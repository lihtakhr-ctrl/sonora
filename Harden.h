#pragma once
#include <ntifs.h>

/* kdmapper等でマップされた場合でも、通常ロードされたドライバと
   同じ内部状態になるよう補正する（EAAC対策） */
VOID HardenDriverObject(PDRIVER_OBJECT drv);

/* PsLoadedModuleList から外して完全ステルス化（任意） */
VOID HideFromModuleList(PDRIVER_OBJECT drv);