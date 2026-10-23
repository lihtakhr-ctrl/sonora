#pragma once
#include <windows.h>

enum class ProfileMode { Auto, ApexRetail, R5Reloaded };

struct Settings
{
    /* --- ESP --- */
    bool  esp       = true;
    bool  box       = true;
    bool  health    = true;
    bool  shield    = true;
    bool  name      = true;
    bool  distance  = true;
    bool  teamCheck = true;

    /* --- Aimbot --- */
    bool  aimbot   = false;
    bool  aimOnKey = true;
    int   aimKey   = VK_XBUTTON2;
    float aimFov   = 120.f;
    float aimSmooth = 6.f;

    /* --- その他 --- */
    int   maxFps = 144;
    bool  menuOpen = true;

    ProfileMode profile  = ProfileMode::Auto;
    bool  preferDriver   = true;   /* false なら ReadProcessMemory を使う */
    bool  spoofEnabled   = true;
    int   spoofSeed      = 0;      /* 0 = 変更しない */
};

extern Settings cfg;