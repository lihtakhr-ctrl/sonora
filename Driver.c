#include <ntifs.h>
#include "..\shared\shared.h"
#include "Spoof.h"
#include "Harden.h"

#define POOL_TAG '0peA'

static PDEVICE_OBJECT g_Device    = NULL;
static PEPROCESS      g_Target    = NULL;
static HANDLE         g_TargetPid = NULL;

/* ---- SystemProcessInformation（バージョン非依存のプロセス列挙） ---- */
typedef struct _SPI {
    ULONG NextEntryOffset, NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount, NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId, InheritedFromUniqueProcessId;
    ULONG HandleCount, SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount;
    LARGE_INTEGER ReadTransferCount, WriteTransferCount, OtherTransferCount;
} SPI, *PSPI;

NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(
    ULONG Class, PVOID Info, ULONG InfoLen, PULONG RetLen);

/* ============================ ヘルパ ============================ */

static NTSTATUS FindProcessByName(PCWSTR name, PULONG outPid)
{
    ULONG needed = 0, size;
    PVOID buf;
    PSPI spi;
    UNICODE_STRING target;
    NTSTATUS st;

    if (!NT_SUCCESS(ZwQuerySystemInformation(5, NULL, 0, &needed)) || !needed)
        return STATUS_UNSUCCESSFUL;

    size = needed + 0x2000;
    buf = ExAllocatePoolWithTag(NonPagedPool, size, POOL_TAG);
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

    st = ZwQuerySystemInformation(5, buf, size, &needed);
    if (!NT_SUCCESS(st)) { ExFreePoolWithTag(buf, POOL_TAG); return st; }

    RtlInitUnicodeString(&target, name);
    spi = (PSPI)buf;
    st  = STATUS_NOT_FOUND;

    for (;;) {
        if (spi->ImageName.Buffer && spi->ImageName.Length &&
            RtlEqualUnicodeString(&spi->ImageName, &target, TRUE)) {
            *outPid = (ULONG)(ULONG_PTR)spi->UniqueProcessId;
            st = STATUS_SUCCESS;
            break;
        }
        if (!spi->NextEntryOffset) break;
        spi = (PSPI)((PUCHAR)spi + spi->NextEntryOffset);
    }

    ExFreePoolWithTag(buf, POOL_TAG);
    return st;
}

static PEPROCESS ResolveTarget(HANDLE pid)
{
    PEPROCESS proc = NULL;
    if (g_Target && g_TargetPid == pid) return g_Target;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(pid, &proc))) return NULL;
    if (g_Target) ObDereferenceObject(g_Target);
    g_Target    = proc;              /* 参照を保持 */
    g_TargetPid = pid;
    return proc;
}

static NTSTATUS CopyMem(HANDLE pid, PVOID addr, PVOID buf, SIZE_T size, BOOLEAN write)
{
    PEPROCESS proc = ResolveTarget(pid);
    SIZE_T done = 0;
    NTSTATUS st;

    if (!proc) return STATUS_INVALID_PARAMETER;

    __try {
        st = write
            ? MmCopyVirtualMemory(PsGetCurrentProcess(), buf, proc, addr, size, KernelMode, &done)
            : MmCopyVirtualMemory(proc, addr, PsGetCurrentProcess(), buf, size, KernelMode, &done);
    } __except (EXCEPTION_EXECUTE_HANDLER) { st = GetExceptionCode(); }

    return st;
}

/* PEB->Ldr を辿ってモジュールベースを取得（x64オフセットはWin10/11で安定） */
static NTSTATUS GetModuleBase(HANDLE pid, PCWSTR moduleName, PULONG64 outBase, PULONG64 outSize)
{
    PEPROCESS proc = ResolveTarget(pid);
    NTSTATUS st = STATUS_NOT_FOUND;
    KAPC_STATE apc;

    if (!proc) return STATUS_INVALID_PARAMETER;

    KeStackAttachProcess(proc, &apc);
    __try {
        PPEB peb = PsGetProcessPeb(proc);
        if (peb && peb->Ldr) {
            PLIST_ENTRY head = &peb->Ldr->InMemoryOrderModuleList;
            PLIST_ENTRY e;
            for (e = head->Flink; e && e != head; e = e->Flink) {
                PUCHAR entry = (PUCHAR)e - 0x10;
                PUNICODE_STRING nm = (PUNICODE_STRING)(entry + 0x58);
                PVOID dllBase = *(PVOID*)(entry + 0x30);
                ULONG imgSize = *(ULONG*)(entry + 0x40);
                UNICODE_STRING target;

                if (!nm || !nm->Buffer || !dllBase) continue;
                RtlInitUnicodeString(&target, moduleName);
                if (RtlEqualUnicodeString(nm, &target, TRUE)) {
                    *outBase = (ULONG64)dllBase;
                    *outSize = (ULONG64)imgSize;
                    st = STATUS_SUCCESS;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { st = GetExceptionCode(); }
    KeUnstackDetachProcess(&apc);

    return st;
}

/* ============================ ディスパッチ ============================ */

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    if (dev != g_Device) return PassThrough(dev, irp);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp;
    PVOID buf;
    ULONG code, inLen;
    NTSTATUS st = STATUS_INVALID_PARAMETER;
    ULONG info = 0;

    if (dev != g_Device) return HookDeviceControl(dev, irp);   /* フィルタ用 */

    sp    = IoGetCurrentIrpStackLocation(irp);
    buf   = irp->AssociatedIrp.SystemBuffer;
    code  = sp->Parameters.DeviceIoControl.IoControlCode;
    inLen = sp->Parameters.DeviceIoControl.InputBufferLength;

    switch (code) {

    case APEX_IOCTL_PING:
        st = STATUS_SUCCESS;
        break;

    case APEX_IOCTL_ATTACH: {
        APEX_PID_REQ* req = (APEX_PID_REQ*)buf;
        PEPROCESS proc = NULL;
        ULONG pid;

        if (inLen < sizeof(APEX_PID_REQ) || !buf) break;
        pid = req->pid;

        if (pid == 0) {
            st = FindProcessByName(req->name, &pid);
            if (!NT_SUCCESS(st)) break;
        }
        st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
        if (!NT_SUCCESS(st)) break;

        if (g_Target) ObDereferenceObject(g_Target);
        g_Target    = proc;
        g_TargetPid = (HANDLE)(ULONG_PTR)pid;
        req->pid    = pid;
        info        = sizeof(APEX_PID_REQ);
        st          = STATUS_SUCCESS;
        break;
    }

    case APEX_IOCTL_GET_MODULE: {
        APEX_MODULE_REQ* req = (APEX_MODULE_REQ*)buf;
        if (inLen < sizeof(APEX_MODULE_REQ) || !buf) break;
        st = GetModuleBase(g_TargetPid, req->name, &req->base, &req->size);
        if (NT_SUCCESS(st)) info = sizeof(APEX_MODULE_REQ);
        break;
    }

    case APEX_IOCTL_READ:
    case APEX_IOCTL_WRITE: {
        APEX_RW* rw = (APEX_RW*)buf;
        if (inLen < sizeof(APEX_RW) || !buf) break;
        if (!rw->size || rw->size > 0x10000000) break;

        __try { ProbeForWrite((PVOID)rw->buffer, (SIZE_T)rw->size, 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { st = GetExceptionCode(); break; }

        st = CopyMem(g_TargetPid, (PVOID)rw->address, (PVOID)rw->buffer,
                     (SIZE_T)rw->size, (BOOLEAN)(code == APEX_IOCTL_WRITE));
        if (NT_SUCCESS(st)) info = sizeof(APEX_RW);
        break;
    }

    case APEX_IOCTL_SPOOF_GET: {
        APEX_SPOOF_INFO* si = (APEX_SPOOF_INFO*)buf;
        if (inLen < sizeof(APEX_SPOOF_INFO) || !buf) break;

        si->enabled      = g_Spoof.Enabled;
        si->seed         = g_Spoof.Seed;
        si->volumeSerial = g_Spoof.VolumeSerial;
        RtlCopyMemory(si->diskSerial,   g_Spoof.DiskSerial,   sizeof(si->diskSerial));
        RtlCopyMemory(si->systemUuid,   g_Spoof.SystemUuid,   sizeof(si->systemUuid));
        RtlCopyMemory(si->systemSerial, g_Spoof.SystemSerial, sizeof(si->systemSerial));
        RtlCopyMemory(si->boardSerial,  g_Spoof.BoardSerial,  sizeof(si->boardSerial));
        RtlCopyMemory(si->mac,          g_Spoof.Mac,          sizeof(si->mac));
        info = sizeof(APEX_SPOOF_INFO);
        st   = STATUS_SUCCESS;
        break;
    }

    case APEX_IOCTL_SPOOF_RESEED: {
        ULONG* s = (ULONG*)buf;
        if (inLen < sizeof(ULONG) || !buf) break;
        SpooferReseed(*s);
        info = sizeof(ULONG);
        st   = STATUS_SUCCESS;
        break;
    }

    case APEX_IOCTL_SPOOF_TOGGLE: {
        BOOLEAN* b = (BOOLEAN*)buf;
        if (inLen < sizeof(BOOLEAN) || !buf) break;
        g_Spoof.Enabled = *b ? TRUE : FALSE;
        if (g_Spoof.Enabled) { SpooferRegistryApply(); SpooferSmbiosApply(); }
        info = sizeof(BOOLEAN);
        st   = STATUS_SUCCESS;
        break;
    }

    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    irp->IoStatus.Status      = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static VOID DriverUnload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING link = RTL_CONSTANT_STRING(APEX_SYMLINK_NAME);
    UNREFERENCED_PARAMETER(drv);

    SpooferUnload();

    IoDeleteSymbolicLink(&link);
    if (g_Device) IoDeleteDevice(g_Device);
    if (g_Target) { ObDereferenceObject(g_Target); g_Target = NULL; }
}

/* ============================ エントリ ============================ */

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    UNICODE_STRING devName = RTL_CONSTANT_STRING(APEX_DEVICE_NAME);
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(APEX_SYMLINK_NAME);
    NTSTATUS st;

    UNREFERENCED_PARAMETER(regPath);

    /* フィルタ用に DeviceExtension を確保しておく */
    st = IoCreateDevice(drv, sizeof(FILTER_EXT), &devName, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, FALSE, &g_Device);
    if (!NT_SUCCESS(st)) return st;

    st = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(g_Device); return st; }

    /* スプーファ初期化（MajorFunction を差し替える） */
    SpooferInit(drv);

    /* コントロールデバイス用ディスパッチを確定 */
    drv->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;

    g_Device->Flags &= ~DO_DEVICE_INITIALIZING;
    drv->DriverUnload = DriverUnload;

    /* EAAC対策 */
    HardenDriverObject(drv);

    return STATUS_SUCCESS;
}