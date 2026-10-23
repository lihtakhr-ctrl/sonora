#pragma once
#include <ntifs.h>
#include "..\shared\shared.h"

typedef struct _SPOOF_PROFILE {
    ULONG   Seed;
    CHAR    DiskSerial[24];
    ULONG   VolumeSerial;
    UCHAR   SystemUuid[16];
    CHAR    SystemSerial[33];
    CHAR    BoardSerial[33];
    UCHAR   Mac[6];
    BOOLEAN Enabled;
} SPOOF_PROFILE;

extern SPOOF_PROFILE g_Spoof;

typedef struct _FILTER_EXT { PDEVICE_OBJECT Lower; } FILTER_EXT, *PFILTER_EXT;

/* フィルタ用ディスパッチ（Driver.c から使う） */
NTSTATUS PassThrough(PDEVICE_OBJECT dev, PIRP irp);
NTSTATUS HookDeviceControl(PDEVICE_OBJECT dev, PIRP irp);

NTSTATUS SpooferInit(PDRIVER_OBJECT OurDriver);
VOID     SpooferUnload(VOID);
VOID     SpooferReseed(ULONG NewSeed);
VOID     SpooferRegistryApply(VOID);
VOID     SpooferSmbiosApply(VOID);