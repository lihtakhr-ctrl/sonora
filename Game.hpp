#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Memory.hpp"
#include "Offsets.hpp"

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Mat4 { float m[4][4]; };

Vec3  operator-(const Vec3& a, const Vec3& b);
float Length(const Vec3& v);
bool  WorldToScreen(const Vec3& w, Vec2& out, const Mat4& vm, int sw, int sh);

struct Player
{
    uint64_t    ptr = 0;
    int         index = 0;
    float       health = 0, shield = 0;
    int         team = 0;
    Vec3        origin{}, head{};
    std::string name;
    float       distance = 0;
};

struct Game
{
    IMemory*              mem = nullptr;
    uint64_t              moduleBase = 0;
    uint64_t              moduleSize = 0;
    std::string           profileName;
    uint64_t              localPlayer = 0;
    Vec3                  localOrigin{}, localCam{};
    int                   localTeam = -1;
    Mat4                  viewMatrix{};
    std::vector<Player>   players;

    bool Attach(IMemory& m, const std::string& forcedProfileId);
    bool Update();
    void RunAimbot(bool enabled, bool onKey, int key, float fov, float smooth);
    bool IsAlive() const;
};

extern Game g_game;