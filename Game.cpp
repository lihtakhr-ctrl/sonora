#include "Game.hpp"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include "Config.hpp"

Game g_game;

Vec3  operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float Length(const Vec3& v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

bool WorldToScreen(const Vec3& w, Vec2& out, const Mat4& vm, int sw, int sh)
{
    float cx = w.x * vm.m[0][0] + w.y * vm.m[1][0] + w.z * vm.m[2][0] + vm.m[3][0];
    float cy = w.x * vm.m[0][1] + w.y * vm.m[1][1] + w.z * vm.m[2][1] + vm.m[3][1];
    float cw = w.x * vm.m[0][3] + w.y * vm.m[1][3] + w.z * vm.m[2][3] + vm.m[3][3];
    if (cw < 0.01f) return false;
    out.x = (sw * 0.5f) * (1.f + cx / cw);
    out.y = (sh * 0.5f) * (1.f - cy / cw);
    return true;
}

bool Game::Attach(IMemory& m, const std::string& forcedProfileId)
{
    mem = &m;

    /* プロセス名が同じ r5apex.exe なので、オフセット検証でプロファイルを判別する */
    std::vector<GameProfile*> order;
    for (auto& p : g_profiles) {
        std::string id(p.id, p.id + wcslen(p.id));
        if (forcedProfileId.empty() || id == forcedProfileId) order.push_back(&p);
    }
    if (order.empty())
        for (auto& p : g_profiles) order.push_back(&p);

    for (GameProfile* prof : order) {
        if (!mem->Attach(prof->process)) continue;

        uint64_t b = 0, s = 0;
        if (!mem->ModuleBase(prof->module, b, s)) continue;

        GameOffsets local;
        if (!AutoResolve(*mem, *prof, b, s, local)) continue;

        goff        = local;
        moduleBase  = b;
        moduleSize  = s;
        profileName = std::string(prof->id, prof->id + wcslen(prof->id));
        return true;
    }
    return false;
}

bool Game::IsAlive() const { return mem && mem->Alive(); }

bool Game::Update()
{
    if (!mem || !mem->Alive()) return false;

    uint32_t handle = MemRead<uint32_t>(*mem, moduleBase + goff.localHandle);
    int idx = (int)(handle & 0xFFFF);
    if (idx <= 0) return false;

    localPlayer = MemRead<uint64_t>(*mem, moduleBase + goff.entityList +
                                    (uint64_t)idx * goff.ENTITY_STRIDE);
    if (localPlayer < 0x10000) return false;

    localTeam   = MemRead<int>(*mem, localPlayer + goff.m_iTeamNum);
    localOrigin = MemRead<Vec3>(*mem, localPlayer + goff.m_vecAbsOrigin);
    localCam    = MemRead<Vec3>(*mem, localPlayer + goff.m_cameraOrigin);

    uint64_t vr = MemRead<uint64_t>(*mem, moduleBase + goff.viewRender);
    if (vr < 0x10000) return false;
    mem->Read(vr + goff.viewMatrix, &viewMatrix, sizeof(Mat4));

    /* エンティティリストを一括読み */
    static std::vector<uint8_t> ebuf;
    ebuf.resize((size_t)goff.MAX_ENTITIES * goff.ENTITY_STRIDE);
    if (!mem->Read(moduleBase + goff.entityList, ebuf.data(), ebuf.size())) return false;

    players.clear();

    for (int i = 1; i < goff.MAX_ENTITIES; ++i) {
        uint64_t ent = *(uint64_t*)(ebuf.data() + (size_t)i * goff.ENTITY_STRIDE);
        uint8_t  probe[0x14]{};
        int health, team;

        if (!ent || ent == localPlayer) continue;
        if (!mem->Read(ent + goff.m_iHealth, probe, sizeof(probe))) continue;

        health = *(int*)probe;
        team   = *(int*)(probe + (goff.m_iTeamNum - goff.m_iHealth));
        if (health <= 0 || health > 200) continue;
        if (team < 0 || team > 100) continue;
        if (cfg.teamCheck && team == localTeam) continue;

        Player p{};
        p.ptr    = ent;
        p.index  = i;
        p.health = (float)health;
        p.team   = team;
        p.shield = (float)MemRead<int>(*mem, ent + goff.m_shieldHealth);
        p.origin = MemRead<Vec3>(*mem, ent + goff.m_vecAbsOrigin);

        uint64_t bones = MemRead<uint64_t>(*mem, ent + goff.m_bones);
        if (bones) {
            struct Bone {
                uint8_t pad[0xcc];
                float x; uint8_t p1[0xC];
                float y; uint8_t p2[0xC];
                float z;
            } b{};
            mem->Read(bones + (uint64_t)goff.HEAD_BONE * goff.BONE_STRIDE, &b, sizeof(b));
            p.head = { b.x + p.origin.x, b.y + p.origin.y, b.z + p.origin.z };
        } else {
            p.head = { p.origin.x, p.origin.y, p.origin.z + 64.f };
        }

        Vec3 d = p.origin - localOrigin;
        p.distance = Length(d) * 0.01905f;   /* Source unit → m */

        if (goff.nameList) {
            uint64_t np = MemRead<uint64_t>(*mem, moduleBase + goff.nameList + (uint64_t)i * 8);
            if (np) {
                char buf[32]{};
                mem->Read(np, buf, sizeof(buf) - 1);
                p.name = buf;
            }
        }
        players.push_back(p);
    }
    return true;
}

void Game::RunAimbot(bool enabled, bool onKey, int key, float fov, float smooth)
{
    if (!enabled || !localPlayer || players.empty()) return;
    if (onKey && !(GetAsyncKeyState(key) & 0x8000)) return;

    extern int g_screenW, g_screenH;
    const float cx = g_screenW * 0.5f, cy = g_screenH * 0.5f;

    float best = fov;
    const Player* target = nullptr;
    for (const Player& p : players) {
        Vec2 s{};
        if (!WorldToScreen(p.head, s, viewMatrix, g_screenW, g_screenH)) continue;
        float d = sqrtf((s.x - cx) * (s.x - cx) + (s.y - cy) * (s.y - cy));
        if (d < best) { best = d; target = &p; }
    }
    if (!target) return;

    Vec3 delta = target->head - localCam;
    float len = Length(delta);
    if (len < 1.f) return;

    float tp = -asinf(delta.z / len) * (180.f / 3.14159265f);
    float ty =  atan2f(delta.y, delta.x) * (180.f / 3.14159265f);

    Vec3 cur = MemRead<Vec3>(*mem, localPlayer + goff.m_viewAngles);
    float dp = tp - cur.x;
    float dy = ty - cur.y;
    while (dy >  180.f) dy -= 360.f;
    while (dy < -180.f) dy += 360.f;

    float s = std::max(1.f, smooth);
    Vec3 next{ cur.x + dp / s, cur.y + dy / s, cur.z };
    mem->Write(localPlayer + goff.m_viewAngles, &next, sizeof(next));
}