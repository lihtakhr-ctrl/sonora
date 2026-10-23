#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <ntddstor.h>
#include <ntdddisk.h>
#include <ntddscsi.h>
#include "Spoof.h"

SPOOF_PROFILE g_Spoof;

static PDRIVER_OBJECT   g_OurDriver;
static PDRIVER_DISPATCH g_OrigDeviceControl;
static PDRIVER_DISPATCH g_OrigQueryVolume;

#define MAX_FILTERS 96
static PDEVICE_OBJECT g_FilterDevs[MAX_FILTERS];
static PFILE_OBJECT   g_VolRefs[MAX_FILTERS];
static ULONG          g_FilterCount;

NTKERNELAPI NTSTATUS ObReferenceObjectByName(
    PUNICODE_STRING ObjectName, ULONG Attributes, PACCESS_STATE PassedAccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType, KPROCESSOR_MODE AccessMode,
    PVOID ParseContext, PVOID* Object);

#define SPOOF_KEY L"\\Registry\\Machine\\SOFTWARE\\VeniceApex"

/* ---------------- プロファイル生成 ---------------- */
static ULONG g_Rng;
static ULONG Rnd(VOID)
{
    g_Rng ^= g_Rng << 13; g_Rng ^= g_Rng >> 17; g_Rng ^= g_Rng << 5;
    return g_Rng;
}
static const CHAR kAlpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";

static VOID BuildProfile(ULONG seed)
{
    g_Rng = seed ? seed : 0x9E3779B9u;
    RtlZeroMemory(&g_Spoof, sizeof(g_Spoof));
    g_Spoof.Seed = seed;

    for (int i = 0; i < 20; i++) g_Spoof.DiskSerial[i] = kAlpha[Rnd() % 34];
    g_Spoof.DiskSerial[20] = 0;

    g_Spoof.VolumeSerial = Rnd();

    for (int i = 0; i < 16; i++) g_Spoof.SystemUuid[i] = (UCHAR)Rnd();
    g_Spoof.SystemUuid[6] = (UCHAR)((g_Spoof.SystemUuid[6] & 0x0F) | 0x40);
    g_Spoof.SystemUuid[8] = (UCHAR)((g_Spoof.SystemUuid[8] & 0x3F) | 0x80);

    for (int i = 0; i < 12; i++) {
        g_Spoof.SystemSerial[i] = kAlpha[Rnd() % 34];
        g_Spoof.BoardSerial[i]  = kAlpha[Rnd() % 34];
    }
    g_Spoof.SystemSerial[12] = g_Spoof.BoardSerial[12] = 0;

    g_Spoof.Mac[0] = 0x02;
    for (int i = 1; i < 6; i++) g_Spoof.Mac[i] = (UCHAR)Rnd();

    g_Spoof.Enabled = TRUE;
}

/* ---------------- シード永続化 ---------------- */
static ULONG LoadSeed(VOID)
{
    UNICODE_STRING kn, vn; OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
    UCHAR buf[64]; ULONG len = 0; ULONG seed = 0;

    RtlInitUnicodeString(&kn, SPOOF_KEY);
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_QUERY_VALUE, &oa))) return 0;

    RtlInitUnicodeString(&vn, L"Seed");
    if (NT_SUCCESS(ZwQueryValueKey(h, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
        PKEY_VALUE_PARTIAL_INFORMATION i = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (i->Type == REG_DWORD && i->DataLength >= 4) RtlCopyMemory(&seed, i->Data, 4);
    }
    ZwClose(h);
    return seed;
}

static VOID SaveSeed(ULONG seed)
{
    UNICODE_STRING kn, vn; OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
    RtlInitUnicodeString(&kn, SPOOF_KEY);
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) return;
    RtlInitUnicodeString(&vn, L"Seed");
    ZwSetValueKey(h, &vn, 0, REG_DWORD, &seed, sizeof(seed));
    ZwClose(h);
}

/* ---------------- 書換ヘルパ ---------------- */
static VOID OverwriteAnsi(PCHAR dst, ULONG maxLen, const CHAR* src)
{
    ULONG orig = 0, n;
    if (!dst || maxLen < 2) return;
    while (orig < maxLen && dst[orig]) orig++;
    if (!orig) return;
    n = (ULONG)strlen(src);
    if (n > orig) n = orig;
    RtlCopyMemory(dst, src, n);
    dst[n] = 0;
}

/* ATA IDENTIFY のシリアルは word10..19 に 2文字ずつリトルエンディアン */
static VOID PatchIdentify(PUSHORT id)
{
    CHAR s[20];
    ULONG n;
    RtlFillMemory(s, 20, ' ');
    n = (ULONG)strlen(g_Spoof.DiskSerial);
    if (n > 20) n = 20;
    RtlCopyMemory(s, g_Spoof.DiskSerial, n);
    for (ULONG i = 0; i < 10; i++)
        id[10 + i] = (USHORT)((UCHAR)s[i * 2] | ((USHORT)(UCHAR)s[i * 2 + 1] << 8));
}

/* ---------------- 完了ルーチン ---------------- */
static VOID PatchDeviceControl(PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG code   = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf    = irp->AssociatedIrp.SystemBuffer;
    if (!buf || outLen < 8) return;

    switch (code) {
    case IOCTL_STORAGE_QUERY_PROPERTY: {
        if (outLen < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return;
        if (((PSTORAGE_PROPERTY_QUERY)buf)->PropertyId != StorageDeviceProperty) return;
        PSTORAGE_DEVICE_DESCRIPTOR d = (PSTORAGE_DEVICE_DESCRIPTOR)buf;
        if (!d->SerialNumberOffset || d->SerialNumberOffset >= outLen) return;
        OverwriteAnsi((PCHAR)buf + d->SerialNumberOffset,
                      outLen - d->SerialNumberOffset, g_Spoof.DiskSerial);
        break;
    }
    case SMART_RCV_DRIVE_DATA:
        if (outLen >= 0x30 + 512) PatchIdentify((PUSHORT)((PUCHAR)buf + 0x30));
        break;

    case IOCTL_ATA_PASS_THROUGH: {
        PATA_PASS_THROUGH_EX a;
        if (outLen < sizeof(ATA_PASS_THROUGH_EX)) return;
        a = (PATA_PASS_THROUGH_EX)buf;
        if (!(a->AtaFlags & ATA_FLAGS_DATA_IN)) return;
        if (a->DataBufferOffset + 512 > outLen) return;
        PatchIdentify((PUSHORT)((PUCHAR)buf + a->DataBufferOffset));
        break;
    }
    case IOCTL_SCSI_PASS_THROUGH: {
        PSCSI_PASS_THROUGH s;
        ULONG page;
        if (outLen < sizeof(SCSI_PASS_THROUGH)) return;
        s = (PSCSI_PASS_THROUGH)buf;
        if (s->Cdb[0] != SCSIOP_INQUIRY || !(s->Cdb[1] & 0x01) || s->Cdb[2] != 0x80) return;
        if (s->DataBufferOffset + 4 > outLen) return;
        page = s->DataTransferLength;
        if (page > 4 && s->DataBufferOffset + page <= outLen)
            OverwriteAnsi((PCHAR)buf + s->DataBufferOffset + 4, page - 4, g_Spoof.DiskSerial);
        break;
    }
    }
}

static NTSTATUS OnDeviceControlDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev); UNREFERENCED_PARAMETER(ctx);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    if (NT_SUCCESS(irp->IoStatus.Status) && g_Spoof.Enabled) PatchDeviceControl(irp);
    return STATUS_CONTINUATION;
}

static NTSTATUS OnQueryVolumeDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    PIO_STACK_LOCATION sp;
    UNREFERENCED_PARAMETER(dev); UNREFERENCED_PARAMETER(ctx);
    if (irp->PendingReturned) IoMarkIrpPending(irp);

    sp = IoGetCurrentIrpStackLocation(irp);
    if (NT_SUCCESS(irp->IoStatus.Status) && g_Spoof.Enabled &&
        sp->Parameters.QueryVolume.FsInformationClass == FileFsVolumeInformation &&
        irp->IoStatus.Information >= sizeof(FILE_FS_VOLUME_INFORMATION))
    {
        PFILE_FS_VOLUME_INFORMATION vi =
            (PFILE_FS_VOLUME_INFORMATION)irp->AssociatedIrp.SystemBuffer;
        if (vi) vi->VolumeSerialNumber = g_Spoof.VolumeSerial;
    }
    return STATUS_CONTINUATION;
}

/* ---------------- ディスパッチ ---------------- */
NTSTATUS PassThrough(PDEVICE_OBJECT dev, PIRP irp)
{
    PFILTER_EXT ext = (PFILTER_EXT)dev->DeviceExtension;
    if (ext && ext->Lower) {
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(ext->Lower, irp);
    }
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS HookDeviceControl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;

    if (!g_Spoof.Enabled ||
        (code != IOCTL_STORAGE_QUERY_PROPERTY && code != SMART_RCV_DRIVE_DATA &&
         code != IOCTL_ATA_PASS_THROUGH && code != IOCTL_SCSI_PASS_THROUGH))
        return PassThrough(dev, irp);

    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, OnDeviceControlDone, NULL, TRUE, TRUE, TRUE);
    return IoCallDriver(((PFILTER_EXT)dev->DeviceExtension)->Lower, irp);
}

static NTSTATUS HookQueryVolume(PDEVICE_OBJECT dev, PIRP irp)
{
    if (!g_Spoof.Enabled) return PassThrough(dev, irp);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, OnQueryVolumeDone, NULL, TRUE, TRUE, TRUE);
    return IoCallDriver(((PFILTER_EXT)dev->DeviceExtension)->Lower, irp);
}

/* ---------------- フィルタアタッチ ---------------- */
static NTSTATUS AttachToDeviceStack(PDEVICE_OBJECT target)
{
    PDEVICE_OBJECT filter = NULL, lower = NULL;
    NTSTATUS st;

    if (g_FilterCount >= MAX_FILTERS) return STATUS_INSUFFICIENT_RESOURCES;

    st = IoCreateDevice(g_OurDriver, sizeof(FILTER_EXT), NULL, target->DeviceType,
                        target->Characteristics, FALSE, &filter);
    if (!NT_SUCCESS(st)) return st;

    filter->Flags |= target->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO |
                                      DO_POWER_PAGABLE | DO_POWER_INRUSH);
    filter->Flags &= ~DO_DEVICE_INITIALIZING;
    filter->AlignmentRequirement = target->AlignmentRequirement;

    st = IoAttachDeviceToDeviceStackSafe(filter, target, &lower);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(filter); return st; }

    ((PFILTER_EXT)filter->DeviceExtension)->Lower = lower;
    g_FilterDevs[g_FilterCount++] = filter;
    return STATUS_SUCCESS;
}

static VOID AttachDiskDriver(VOID)
{
    UNICODE_STRING n; PDRIVER_OBJECT drv = NULL; PDEVICE_OBJECT d;
    RtlInitUnicodeString(&n, L"\\Driver\\disk");
    if (!NT_SUCCESS(ObReferenceObjectByName(&n, OBJ_CASE_INSENSITIVE, NULL, 0,
                                            *IoDriverObjectType, KernelMode, NULL, (PVOID*)&drv)))
        return;
    for (d = drv->DeviceObject; d; d = d->NextDevice)
        if (d->DeviceType == FILE_DEVICE_DISK || d->DeviceType == FILE_DEVICE_VIRTUAL_DISK)
            AttachToDeviceStack(d);
    ObDereferenceObject(drv);
}

static VOID AttachVolumes(VOID)
{
    for (ULONG i = 1; i <= 16 && g_FilterCount < MAX_FILTERS; i++) {
        WCHAR name[64];
        UNICODE_STRING n;
        PFILE_OBJECT fo = NULL;
        PDEVICE_OBJECT dev = NULL;

        RtlStringCchPrintfW(name, 64, L"\\Device\\HarddiskVolume%u", i);
        RtlInitUnicodeString(&n, name);
        if (!NT_SUCCESS(IoGetDeviceObjectPointer(&n, FILE_READ_ATTRIBUTES, &fo, &dev))) continue;

        if (NT_SUCCESS(AttachToDeviceStack(dev)) && fo)
            g_VolRefs[g_FilterCount - 1] = fo;
        else if (fo)
            ObDereferenceObject(fo);
    }
}

/* ---------------- SMBIOS パッチ ---------------- */
static VOID PatchSmbiosString(PUCHAR start, UCHAR len, UCHAR index, const CHAR* str, PUCHAR end)
{
    PUCHAR p;
    ULONG avail, n;
    if (!index) return;

    p = start + len;
    for (UCHAR i = 1; i < index && p < end; i++) {
        while (p < end && *p) p++;
        p++;
    }
    if (p >= end || !*p) return;

    avail = 0;
    while (p + avail < end && p[avail]) avail++;
    if (!avail) return;

    n = (ULONG)strlen(str);
    if (n > avail) n = avail;
    RtlCopyMemory(p, str, n);
    p[n] = 0;
}

VOID SpooferSmbiosApply(VOID)
{
    UNICODE_STRING kn, vn; OBJECT_ATTRIBUTES oa; HANDLE h = NULL;
    UCHAR buf[512]; ULONG len = 0;
    PKEY_VALUE_PARTIAL_INFORMATION info;
    PCM_RESOURCE_LIST rl;
    PHYSICAL_ADDRESS phys; ULONG size;
    PVOID m; PUCHAR p;

    if (!g_Spoof.Enabled) return;

    RtlInitUnicodeString(&kn, L"\\Registry\\Machine\\HARDWARE\\RESOURCEMAP\\System Resources\\Loader Reserved");
    InitializeObjectAttributes(&oa, &kn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_QUERY_VALUE, &oa))) return;

    RtlInitUnicodeString(&vn, L"RsMB");
    if (!NT_SUCCESS(ZwQueryValueKey(h, &vn, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
        ZwClose(h); return;
    }
    ZwClose(h);

    info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (info->Type != REG_RESOURCE_LIST || info->DataLength < sizeof(CM_RESOURCE_LIST)) return;

    rl   = (PCM_RESOURCE_LIST)info->Data;
    phys = rl->List[0].PartialResourceList.PartialDescriptors[0].u.Memory.Start;
    size = rl->List[0].PartialResourceList.PartialDescriptors[0].u.Memory.Length;
    if (!phys.QuadPart || size < 0x100 || size > 0x100000) return;

    m = MmMapIoSpace(phys, size, MmNonCached);
    if (!m) return;

    p = (PUCHAR)m;
    __try {
        if (p[0] == '_' && p[1] == 'S' && p[2] == 'M' && p[3] == '_') {
            PUCHAR table = p + *(PUSHORT)(p + 6);
            ULONG  tlen  = *(PULONG)(p + 0x1C);
            PUCHAR s, end;

            if (tlen > size) tlen = size;
            s = table; end = table + tlen;

            while (s + 4 < end) {
                UCHAR type = s[0], slen = s[1];
                if (slen < 4) break;
                if (type == 1 && slen >= 0x19) {
                    RtlCopyMemory(s + 0x08, g_Spoof.SystemUuid, 16);
                    PatchSmbiosString(s, slen, s[0x07], g_Spoof.SystemSerial, end);
                    break;
                }
                if (type == 127) break;
                {
                    PUCHAR q = s + slen;
                    while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
                    q += 2;
                    if (q <= s) break;
                    s = q;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }

    MmUnmapIoSpace(m, size);
}

/* ---------------- レジストリ ---------------- */
static VOID WriteRegSz(PCWSTR key, PCWSTR value, PCWSTR data)
{
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, key, value, REG_SZ,
                          (PVOID)data, (ULONG)((wcslen(data) + 1) * sizeof(WCHAR)));
}

VOID SpooferRegistryApply(VOID)
{
    WCHAR serial[40], board[40];
    ANSI_STRING a; UNICODE_STRING u;

    if (!g_Spoof.Enabled) return;

    RtlInitAnsiString(&a, g_Spoof.SystemSerial);
    u.Buffer = serial; u.MaximumLength = sizeof(serial); u.Length = 0;
    RtlAnsiStringToUnicodeString(&u, &a, FALSE);

    RtlInitAnsiString(&a, g_Spoof.BoardSerial);
    u.Buffer = board; u.MaximumLength = sizeof(board); u.Length = 0;
    RtlAnsiStringToUnicodeString(&u, &a, FALSE);

    WriteRegSz(L"\\Registry\\Machine\\HARDWARE\\DESCRIPTION\\System\\BIOS",
               L"SystemSerialNumber", serial);
    WriteRegSz(L"\\Registry\\Machine\\HARDWARE\\DESCRIPTION\\System\\BIOS",
               L"BaseBoardSerialNumber", board);
    WriteRegSz(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\SystemInformation",
               L"SystemSerialNumber", serial);
    WriteRegSz(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\SystemInformation",
               L"BaseBoardSerialNumber", board);

    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\Disk\\Enum",
        L"0", REG_SZ, (PVOID)L"0", 4);
}

/* ---------------- 初期化 / 解放 ---------------- */
NTSTATUS SpooferInit(PDRIVER_OBJECT OurDriver)
{
    ULONG seed;

    g_OurDriver = OurDriver;

    seed = LoadSeed();
    if (!seed) {
        seed = (ULONG)KeQueryPerformanceCounter(NULL).QuadPart;
        if (!seed) seed = 1;
        SaveSeed(seed);
    }
    BuildProfile(seed);

    g_OrigDeviceControl = OurDriver->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    g_OrigQueryVolume   = OurDriver->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION];

    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        OurDriver->MajorFunction[i] = PassThrough;

    OurDriver->MajorFunction[IRP_MJ_DEVICE_CONTROL]           = HookDeviceControl;
    OurDriver->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = HookQueryVolume;

    AttachDiskDriver();
    AttachVolumes();

    SpooferRegistryApply();
    SpooferSmbiosApply();

    return STATUS_SUCCESS;
}

VOID SpooferUnload(VOID)
{
    for (ULONG i = 0; i < g_FilterCount; i++) {
        PDEVICE_OBJECT f = g_FilterDevs[i];
        PFILTER_EXT ext;
        if (!f) continue;
        ext = (PFILTER_EXT)f->DeviceExtension;
        if (ext->Lower) IoDetachDevice(ext->Lower);
        IoDeleteDevice(f);
        if (g_VolRefs[i]) { ObDereferenceObject(g_VolRefs[i]); g_VolRefs[i] = NULL; }
    }
    g_FilterCount = 0;

    if (g_OurDriver) {
        g_OurDriver->MajorFunction[IRP_MJ_DEVICE_CONTROL]           = g_OrigDeviceControl;
        g_OurDriver->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = g_OrigQueryVolume;
    }
}

VOID SpooferReseed(ULONG NewSeed)
{
    if (!NewSeed) return;
    BuildProfile(NewSeed);
    SaveSeed(NewSeed);
    SpooferRegistryApply();
    SpooferSmbiosApply();
}