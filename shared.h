#pragma once
/* ============================================================
 *  ApexRing0 - カーネル / ユーザーモード共有定義
 *  ドライバとクライアントの両方から include する
 * ============================================================ */
#include <stdint.h>

#ifdef _KERNEL_MODE
    #include <ntddk.h>
    typedef WCHAR apex_char;
#else
    typedef wchar_t apex_char;
#endif

#define APEX_DEVICE_NAME   L"\\Device\\ApexRing0"
#define APEX_SYMLINK_NAME  L"\\DosDevices\\ApexRing0"
#define APEX_USER_PATH     L"\\\\.\\ApexRing0"

#define APEX_IOCTL_PING         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_ATTACH       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_GET_MODULE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_READ         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_WRITE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_SPOOF_GET    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_SPOOF_RESEED CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define APEX_IOCTL_SPOOF_TOGGLE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)

/* 読み書き要求。buffer は「呼び出し元プロセスのユーザーVA」 */
typedef struct _APEX_RW {
    uint64_t address;
    uint64_t buffer;
    uint64_t size;
} APEX_RW;

typedef struct _APEX_MODULE_REQ {
    apex_char name[64];
    uint64_t  base;
    uint64_t  size;
} APEX_MODULE_REQ;

typedef struct _APEX_PID_REQ {
    apex_char name[64];     /* 0 ならプロセス名で検索 */
    uint32_t  pid;
} APEX_PID_REQ;

typedef struct _APEX_SPOOF_INFO {
    uint32_t enabled;
    uint32_t seed;
    char     diskSerial[24];
    uint32_t volumeSerial;
    uint8_t  systemUuid[16];
    char     systemSerial[33];
    char     boardSerial[33];
    uint8_t  mac[6];
    uint16_t reserved;
} APEX_SPOOF_INFO;

#pragma pack(pop)