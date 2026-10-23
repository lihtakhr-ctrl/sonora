#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct GameOffsets
{
    /* モジュール相対ポインタ */
    uint64_t entityList = 0, localHandle = 0, viewRender = 0, viewMatrix = 0;
    uint64_t nameList = 0, netVarTable = 0;

    /* エンティティ相対フィールド */
    uint64_t m_vecAbsOrigin = 0x170;
    uint64_t m_iHealth      = 0x330;
    uint64_t m_iTeamNum     = 0x340;
    uint64_t m_lifeState    = 0x698;
    uint64_t m_shieldHealth = 0x18c;
    uint64_t m_shieldMax    = 0x190;
    uint64_t m_bones        = 0xde8;
    uint64_t m_cameraOrigin = 0x1f74;
    uint64_t m_viewAngles   = 0x25c0;

    /* レイアウト */
    uint64_t ENTITY_STRIDE = 0x20;
    uint64_t BONE_STRIDE   = 0x30;
    uint64_t BONE_POS      = 0xcc;
    int      HEAD_BONE     = 8;
    int      MAX_ENTITIES  = 0x10000;
};

struct GameProfile
{
    const wchar_t* id;
    const wchar_t* process;
    const wchar_t* module;
    GameOffsets    defaults;
    bool           useSignatures;
    bool           useRemoteDb;
};

extern GameOffsets              goff;
extern std::vector<GameProfile> g_profiles;

/* 自動解決（AutoOffsets.cpp） */
bool AutoResolve(class IMemory& mem, const GameProfile& prof,
                 uint64_t moduleBase, uint64_t moduleSize, GameOffsets& out);
bool ValidateOffsets(IMemory& mem, uint64_t base, const GameOffsets& o);