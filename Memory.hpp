#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <string>
#include <vector>
#include "shared.h"

/* ============================================================
 *  メモリバックエンド抽象化
 *   KernelMem  : ドライバ経由（EAAC対策・高速）
 *   HandleMem  : OpenProcess + RPM（r5r等アンチチート無し向け）
 * ============================================================ */
struct IMemory
{
    virtual ~IMemory() {}
    virtual bool Read (uint64_t addr, void* out, size_t size) = 0;
    virtual bool Write(uint64_t addr, const void* in, size_t size) = 0;
    virtual bool ModuleBase(const wchar_t* name, uint64_t& base, uint64_t& size) = 0;
    virtual bool Attach(const wchar_t* procName) = 0;
    virtual DWORD Pid() const = 0;
    virtual bool Alive() const = 0;
};

template <class T> T MemRead(IMemory& m, uint64_t a)
{
    T v{};
    m.Read(a, &v, sizeof(T));
    return v;
}

/* ---------------- ドライバ経由 ---------------- */
class KernelMem : public IMemory
{
public:
    ~KernelMem() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

    bool Open()
    {
        h_ = CreateFileW(APEX_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        APEX_PID_REQ req{};
        DWORD ret = 0;
        return DeviceIoControl(h_, APEX_IOCTL_PING, &req, sizeof(req),
                               &req, sizeof(req), &ret, nullptr) != 0;
    }

    bool Attach(const wchar_t* procName) override
    {
        APEX_PID_REQ req{};
        wcsncpy_s(req.name, procName, _TRUNCATE);
        DWORD ret = 0;
        if (!DeviceIoControl(h_, APEX_IOCTL_ATTACH, &req, sizeof(req),
                             &req, sizeof(req), &ret, nullptr)) return false;
        pid_ = req.pid;
        return pid_ != 0;
    }

    bool ModuleBase(const wchar_t* name, uint64_t& base, uint64_t& size) override
    {
        APEX_MODULE_REQ req{};
        wcsncpy_s(req.name, name, _TRUNCATE);
        DWORD ret = 0;
        if (!DeviceIoControl(h_, APEX_IOCTL_GET_MODULE, &req, sizeof(req),
                             &req, sizeof(req), &ret, nullptr)) return false;
        base = req.base; size = req.size;
        return base != 0;
    }

    bool Read(uint64_t addr, void* out, size_t size) override
    {
        if (!addr || !size || size > 0x10000000) return false;
        APEX_RW rw{ addr, (uint64_t)out, size };
        DWORD ret = 0;
        return DeviceIoControl(h_, APEX_IOCTL_READ, &rw, sizeof(rw),
                               &rw, sizeof(rw), &ret, nullptr) != 0;
    }

    bool Write(uint64_t addr, const void* in, size_t size) override
    {
        if (!addr || !size || size > 0x10000000) return false;
        APEX_RW rw{ addr, (uint64_t)in, size };
        DWORD ret = 0;
        return DeviceIoControl(h_, APEX_IOCTL_WRITE, &rw, sizeof(rw),
                               &rw, sizeof(rw), &ret, nullptr) != 0;
    }

    bool SpoofInfo(APEX_SPOOF_INFO& out)
    {
        DWORD ret = 0;
        return DeviceIoControl(h_, APEX_IOCTL_SPOOF_GET, &out, sizeof(out),
                               &out, sizeof(out), &ret, nullptr) != 0;
    }
    bool SpoofReseed(uint32_t seed)
    {
        DWORD ret = 0;
        return DeviceIoControl(h_, APEX_IOCTL_SPOOF_RESEED, &seed, sizeof(seed),
                               &seed, sizeof(seed), &ret, nullptr) != 0;
    }
    bool SpoofToggle(bool on)
    {
        BOOLEAN b = on ? 1 : 0; DWORD ret = 0;
        return DeviceIoControl(h_, APEX_IOCTL_SPOOF_TOGGLE, &b, sizeof(b),
                               &b, sizeof(b), &ret, nullptr) != 0;
    }

    DWORD Pid() const override { return pid_; }
    bool  Alive() const override { return pid_ != 0; }

private:
    HANDLE h_   = INVALID_HANDLE_VALUE;
    DWORD  pid_ = 0;
};

/* ---------------- RPM フォールバック ---------------- */
class HandleMem : public IMemory
{
public:
    ~HandleMem() { if (h_) CloseHandle(h_); }

    bool Attach(const wchar_t* procName) override
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        bool found = false;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, procName) == 0) {
                    pid_ = pe.th32ProcessID; found = true; break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (!found) return false;

        h_ = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE |
                         PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
                         FALSE, pid_);
        return h_ != nullptr;
    }

    bool ModuleBase(const wchar_t* name, uint64_t& base, uint64_t& size) override
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
        if (snap == INVALID_HANDLE_VALUE) return false;

        MODULEENTRY32W me{}; me.dwSize = sizeof(me);
        bool found = false;
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, name) == 0) {
                    base = (uint64_t)me.modBaseAddr;
                    size = me.modBaseSize;
                    found = true;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    bool Read(uint64_t addr, void* out, size_t size) override
    {
        SIZE_T got = 0;
        return ReadProcessMemory(h_, (LPCVOID)addr, out, size, &got) != 0;
    }

    bool Write(uint64_t addr, const void* in, size_t size) override
    {
        SIZE_T put = 0;
        return WriteProcessMemory(h_, (LPVOID)addr, in, size, &put) != 0;
    }

    DWORD Pid() const override { return pid_; }
    bool  Alive() const override { return h_ != nullptr; }

private:
    HANDLE h_   = nullptr;
    DWORD  pid_ = 0;
};