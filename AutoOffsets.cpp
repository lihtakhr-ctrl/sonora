#include "Offsets.hpp"
#include "Memory.hpp"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "winhttp.lib")

GameOffsets goff;

/* ============================================================
 *  プロファイル定義
 *  ポインタはシグネチャで上書きされるので、ここはフィールドの初期値が主
 * ============================================================ */
std::vector<GameProfile> g_profiles = {
    {
        L"Apex Legends (retail / EAAC)",
        L"r5apex.exe", L"r5apex.exe",
        GameOffsets{},      /* 既定値を使用 */
        true, true
    },
    {
        L"R5Reloaded (modded, no AC)",
        L"r5apex.exe", L"r5apex.exe",
        GameOffsets{},
        true, false         /* r5rはリモートDBを使わない */
    },
};

/* ============================================================
 *  シグネチャ（エンジン共通なので両プロファイルで有効）
 * ============================================================ */
static const char* SIG_ENTITY_LIST =
    "8B 90 28 ? ? ? 83 FA FF 74 ? 0F B7 C2 48 8D 0D ? ? ? ? 48 C1 E0 ? C1 EA";
static const char* SIG_LOCAL_HANDLE =
    "83 3D ? ? ? ? FF 74 ? 0F B7 0D ? ? ? ? 0F B7 05 ? ? ? ? 48 C1 E1 ? 48 03 CA";
static const char* SIG_VIEW_RENDER =
    "48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? 8B ? ? ? ? ? ? 8B ? ? ? ? ? ? 48 8B 01 48 3B C2 0F 85 ? ? ? ? 48 8B 81 ? ? ? ?";

/* ============================================================
 *  パターンスキャナ
 * ============================================================ */
class Scanner
{
public:
    bool Load(IMemory& m, uint64_t base, uint64_t size)
    {
        m_base = base; m_size = size;
        buf_.assign((size_t)size, 0);
        const uint64_t CHUNK = 8ull << 20;
        bool any = false;
        for (uint64_t o = 0; o < size; o += CHUNK) {
            size_t n = (size_t)std::min<uint64_t>(CHUNK, size - o);
            if (m.Read(base + o, buf_.data() + o, n)) any = true;
        }
        return any;
    }

    uint64_t Find(const char* sig) const
    {
        std::vector<uint8_t> b, k;
        if (!Parse(sig, b, k)) return 0;
        const size_t n = b.size();
        for (uint64_t i = 0; i + n <= m_size; ++i) {
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (k[j] && buf_[(size_t)(i + j)] != b[j]) { ok = false; break; }
            if (ok) return m_base + i;
        }
        return 0;
    }

    int32_t ReadI32(uint64_t abs) const
    {
        if (abs < m_base || abs + 4 > m_base + m_size) return 0;
        int32_t v = 0;
        memcpy(&v, buf_.data() + (abs - m_base), 4);
        return v;
    }

private:
    static bool Parse(const char* s, std::vector<uint8_t>& b, std::vector<uint8_t>& k)
    {
        b.clear(); k.clear();
        for (const char* p = s; *p; ) {
            while (*p == ' ') ++p;
            if (!*p) break;
            if (*p == '?') { b.push_back(0); k.push_back(0); ++p; if (*p == '?') ++p; }
            else { char* e = nullptr; b.push_back((uint8_t)strtol(p, &e, 16)); k.push_back(1); p = e; }
        }
        return !b.empty();
    }
    std::vector<uint8_t> buf_;
    uint64_t m_base = 0, m_size = 0;
};

/* ============================================================
 *  リモートオフセットDB（retailのみ）
 * ============================================================ */
static std::string HttpGet(const std::string& url)
{
    std::string out;
    std::wstring wurl(url.begin(), url.end());

    HINTERNET s = WinHttpOpen(L"VeniceApex/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return out;

    URL_COMPONENTS uc{};
    wchar_t host[256]{}, path[1024]{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { WinHttpCloseHandle(s); return out; }

    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (!c) { WinHttpCloseHandle(s); return out; }

    DWORD fl = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, fl);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return out; }

    WinHttpSetTimeouts(r, 4000, 4000, 4000, 8000);
    if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr))
    {
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
            std::vector<char> ch(avail + 1, 0);
            DWORD rd = 0;
            if (!WinHttpReadData(r, ch.data(), avail, &rd)) break;
            out.append(ch.data(), rd);
        } while (avail);
    }

    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return out;
}

static bool TryRemoteDb(GameOffsets& o)
{
    const char* URL   = "https://www.cheatoffsets.com/g/apex/raw";
    const char* CACHE = "offsets_cache.txt";

    std::string body;
    std::ifstream f(CACHE);
    if (f) {
        std::time_t stamp = 0;
        f >> stamp;
        if (std::time(nullptr) - stamp < 24 * 3600)
            body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (body.empty()) {
        body = HttpGet(URL);
        if (!body.empty()) {
            std::ofstream o2(CACHE);
            o2 << std::time(nullptr) << "\n" << body;
        }
    }
    if (body.empty()) return false;

    static const std::regex re(R"("?([A-Za-z_][A-Za-z0-9_]*)"?\s*[:=]\s*"?0x([0-9A-Fa-f]+)"?)");
    int hits = 0;

    for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
         it != std::sregex_iterator(); ++it)
    {
        const std::smatch& m = *it;
        std::string key = m[1].str();
        uint64_t v = 0;
        try { v = std::stoull(m[2].str(), nullptr, 16); } catch (...) { continue; }
        ++hits;

        if      (key == "cl_entitylist" || key == "dwEntityList")        o.entityList   = v;
        else if (key == "LocalEntityHandle" || key == "dwLocalEntityHandle") o.localHandle = v;
        else if (key == "ViewRender")                                    o.viewRender   = v;
        else if (key == "ViewMatrix")                                    o.viewMatrix   = v;
        else if (key == "NameList")                                      o.nameList     = v;
        else if (key == "NetworkVarTablePtr")                            o.netVarTable  = v;
        else if (key == "m_vecAbsOrigin")                                o.m_vecAbsOrigin = v;
        else if (key == "m_iHealth")                                     o.m_iHealth    = v;
        else if (key == "m_iTeamNum")                                    o.m_iTeamNum   = v;
        else if (key == "m_lifeState")                                   o.m_lifeState  = v;
        else if (key == "m_shieldHealth")                                o.m_shieldHealth = v;
        else if (key == "m_shieldHealthMax")                             o.m_shieldMax  = v;
        else if (key == "m_nForceBone")                                  o.m_bones      = v + 0x48;
        else if (key == "camera_origin" || key == "m_cameraOrigin")      o.m_cameraOrigin = v;
        else if (key == "ViewAngles")                                    o.m_viewAngles = v;
    }
    return hits >= 4;
}

/* ============================================================
 *  NetVar からフィールドオフセットを名前で自動解決
 *  ・トップレベル名でハッシュ関数を自己検証してから使う
 *  ・ポインタはプロファイル依存なので失敗したら何もしない
 * ============================================================ */
static uint64_t Fnv1a(const char* s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (*s) { h ^= (unsigned char)*s++; h *= 0x100000001B3ull; }
    return h;
}

static std::string ReadCStr(IMemory& m, uint64_t addr, size_t max = 64)
{
    char b[128]{};
    if (max > sizeof(b)) max = sizeof(b);
    m.Read(addr, b, max);
    b[max - 1] = 0;
    return std::string(b);
}

static bool TryNetVars(IMemory& mem, uint64_t base, GameOffsets& o)
{
    if (!o.netVarTable) return false;
    uint64_t tbl = mem.Read<uint64_t>(base + o.netVarTable);
    if (tbl < 0x10000) return false;

    /* --- 1) ハッシュ関数の自己検証 --- */
    int verified = 0;
    for (int i = 0; i < 8; ++i) {
        uint64_t e = tbl + (uint64_t)i * 0x20;
        uint64_t stored = mem.Read<uint64_t>(e);
        std::string nm = ReadCStr(mem, mem.Read<uint64_t>(e + 0x08));
        if (nm.empty()) break;
        if (Fnv1a(nm.c_str()) == stored) ++verified;
    }
    if (verified < 5) return false;

    /* --- 2) 対象フィールドをハッシュで検索 --- */
    struct Target { const char* name; uint64_t* out; };
    Target targets[] = {
        { "m_iHealth",      &o.m_iHealth      },
        { "m_iTeamNum",     &o.m_iTeamNum     },
        { "m_vecAbsOrigin", &o.m_vecAbsOrigin },
        { "m_lifeState",    &o.m_lifeState    },
        { "m_shieldHealth", &o.m_shieldHealth },
        { "m_nForceBone",   &o.m_bones        },
    };

    for (int i = 0; i < 512; ++i) {
        uint64_t e = tbl + (uint64_t)i * 0x20;
        uint64_t sub = mem.Read<uint64_t>(e + 0x10);
        uint32_t cnt = mem.Read<uint32_t>(e + 0x18);
        if (!sub || !cnt || cnt > 0x2000) break;

        for (uint32_t k = 0; k < cnt; ++k) {
            uint64_t se = sub + (uint64_t)k * 0x10;
            uint64_t h  = mem.Read<uint64_t>(se);
            uint32_t off = mem.Read<uint32_t>(se + 0x08);
            if (!off) continue;
            for (auto& t : targets)
                if (h == Fnv1a(t.name)) *t.out = off;
        }
    }
    if (o.m_bones) o.m_bones += 0x48;   /* m_nForceBone → ボーン配列 */
    return true;
}

/* ============================================================
 *  シグネチャ解決
 * ============================================================ */
static void TrySignatures(IMemory& mem, uint64_t base, uint64_t size, GameOffsets& o)
{
    Scanner sc;
    if (!sc.Load(mem, base, size)) return;

    struct { const char* sig; int rel; int len; uint64_t* out; } tbl[] = {
        { SIG_ENTITY_LIST,  17, 21, &o.entityList  },
        { SIG_LOCAL_HANDLE,  2,  7, &o.localHandle },
        { SIG_VIEW_RENDER,   3,  7, &o.viewRender  },
    };
    for (auto& t : tbl) {
        uint64_t hit = sc.Find(t.sig);
        if (!hit) continue;
        uint64_t target = hit + t.len + sc.ReadI32(hit + t.rel);
        if (target > base && target < base + size)
            *t.out = target - base;
    }

    uint64_t hit = sc.Find(SIG_VIEW_RENDER);
    if (hit) {
        int32_t vm = sc.ReadI32(hit + 43);
        if (vm > 0 && vm < 0x400000) o.viewMatrix = (uint64_t)vm;
    }
}

/* ============================================================
 *  検証（ビュー行列の数学的整合性でプロファイルを判定）
 * ============================================================ */
bool ValidateOffsets(IMemory& mem, uint64_t base, const GameOffsets& o)
{
    float m[16]{};
    uint64_t vr, local;
    uint32_t h;
    int idx, hp;

    if (!o.entityList || !o.localHandle || !o.viewRender) return false;

    vr = mem.Read<uint64_t>(base + o.viewRender);
    if (vr < 0x10000) return false;
    if (!mem.Read(vr + o.viewMatrix, m, sizeof(m))) return false;

    /* Source の VMatrix は最終行が (0,0,0,1) */
    if (fabsf(m[15] - 1.0f) > 0.02f) return false;
    if (fabsf(m[3]) > 0.02f || fabsf(m[7]) > 0.02f || fabsf(m[11]) > 0.02f) return false;

    h   = mem.Read<uint32_t>(base + o.localHandle);
    idx = (int)(h & 0xFFFF);
    if (idx <= 0 || idx >= o.MAX_ENTITIES) return false;

    local = mem.Read<uint64_t>(base + o.entityList + (uint64_t)idx * o.ENTITY_STRIDE);
    if (local < 0x10000) return false;

    hp = mem.Read<int>(local + o.m_iHealth);
    return hp >= 0 && hp <= 200;
}

/* ============================================================
 *  統合エントリ
 * ============================================================ */
bool AutoResolve(IMemory& mem, const GameProfile& prof,
                 uint64_t moduleBase, uint64_t moduleSize, GameOffsets& out)
{
    out = prof.defaults;

    if (prof.useRemoteDb) TryRemoteDb(out);
    if (prof.useSignatures) TrySignatures(mem, moduleBase, moduleSize, out);
    TryNetVars(mem, moduleBase, out);

    if (!ValidateOffsets(mem, moduleBase, out)) {
        printf("[offset] '%S' で検証失敗\n", prof.id);
        return false;
    }

    printf("[offset] '%S' OK  ent=0x%llX handle=0x%llX view=0x%llX vm=0x%llX hp=0x%llX\n",
           prof.id, out.entityList, out.localHandle, out.viewRender, out.viewMatrix, out.m_iHealth);
    return true;
}