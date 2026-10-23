#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <cstdio>
#include <memory>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "Config.hpp"
#include "Memory.hpp"
#include "Offsets.hpp"
#include "Game.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

Settings cfg;

int g_screenW = 1920, g_screenH = 1080;

static std::unique_ptr<IMemory> g_mem;

/* ===================== D3D11 ===================== */
static ID3D11Device*           g_dev  = nullptr;
static ID3D11DeviceContext*    g_ctx  = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;
static HWND                    g_hwnd = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return true;

    if (msg == WM_KEYDOWN && w == VK_INSERT) {
        cfg.menuOpen = !cfg.menuOpen;
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (cfg.menuOpen) ex &= ~WS_EX_TRANSPARENT;
        else              ex |=  WS_EX_TRANSPARENT;
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
    }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, msg, w, l);
}

static bool CreateOverlay()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VeniceOverlayWnd";
    RegisterClassExW(&wc);

    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"Venice Overlay", WS_POPUP,
        0, 0, g_screenW, g_screenH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;

    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width  = g_screenW;
    sd.BufferDesc.Height = g_screenH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL lvl;
    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             lvls, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                             &g_dev, &lvl, &g_ctx)))
        return false;

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    if (back) back->Release();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    return true;
}

/* ===================== ESP ===================== */
static void DrawESP()
{
    if (!cfg.esp) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float cx = g_screenW * 0.5f, cy = g_screenH * 0.5f;

    for (const Player& p : g_game.players) {
        Vec2 sh{}, sf{};
        if (!WorldToScreen(p.head,   sh, g_game.viewMatrix, g_screenW, g_screenH)) continue;
        if (!WorldToScreen(p.origin, sf, g_game.viewMatrix, g_screenW, g_screenH)) continue;

        float h = sf.y - sh.y;
        if (h < 2.f || h > 2000.f) continue;
        float w = h * 0.5f;
        float x = sh.x - w * 0.5f, y = sh.y;

        ImU32 col = (p.team == g_game.localTeam) ? IM_COL32(0, 255, 120, 220)
                                                 : IM_COL32(255, 80, 80, 230);
        if (cfg.box) dl->AddRect({ x, y }, { x + w, y + h }, col, 0.f, 0, 1.5f);

        if (cfg.health) {
            float hp = std::clamp(p.health / 100.f, 0.f, 1.f);
            dl->AddRectFilled({ x - 6, y + h * (1.f - hp) }, { x - 2, y + h },
                              IM_COL32(60, 255, 60, 220));
        }
        if (cfg.shield && p.shield > 0.f) {
            float sv = std::clamp(p.shield / 100.f, 0.f, 1.f);
            dl->AddRectFilled({ x - 11, y + h * (1.f - sv) }, { x - 7, y + h },
                              IM_COL32(90, 170, 255, 220));
        }
        if (cfg.name && !p.name.empty()) {
            ImVec2 sz = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText({ sh.x - sz.x * 0.5f, y - 16 }, IM_COL32(255, 255, 255, 235), p.name.c_str());
        }
        if (cfg.distance) {
            char b[32];
            sprintf_s(b, "[%.0fm]", p.distance);
            ImVec2 sz = ImGui::CalcTextSize(b);
            dl->AddText({ sh.x - sz.x * 0.5f, y + h + 2 }, IM_COL32(230, 230, 230, 220), b);
        }
    }

    if (cfg.aimbot)
        dl->AddCircle({ cx, cy }, cfg.aimFov, IM_COL32(255, 255, 255, 90), 64, 1.2f);
}

/* ===================== メニュー ===================== */
static void DrawMenu()
{
    if (!cfg.menuOpen) return;

    ImGui::SetNextWindowSize({ 400, 500 }, ImGuiCond_FirstUseEver);
    ImGui::Begin("Venice Apex Ring0", &cfg.menuOpen);

    ImGui::Text("FPS %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Profile: %s", g_game.profileName.c_str());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("ESP", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable",     &cfg.esp);
        ImGui::Checkbox("Box",        &cfg.box);
        ImGui::Checkbox("Health",     &cfg.health);
        ImGui::Checkbox("Shield",     &cfg.shield);
        ImGui::Checkbox("Name",       &cfg.name);
        ImGui::Checkbox("Distance",   &cfg.distance);
        ImGui::Checkbox("Team check", &cfg.teamCheck);
    }
    if (ImGui::CollapsingHeader("Aimbot", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable",    &cfg.aimbot);
        ImGui::Checkbox("Hold key",  &cfg.aimOnKey);
        ImGui::SliderFloat("FOV",    &cfg.aimFov,    5.f, 600.f, "%.0f px");
        ImGui::SliderFloat("Smooth", &cfg.aimSmooth, 1.f,  30.f, "%.1f");
    }
    if (ImGui::CollapsingHeader("HWID Spoofer (temp)")) {
        static APEX_SPOOF_INFO si{};
        static bool fetched = false;
        if (!fetched) { auto* km = dynamic_cast<KernelMem*>(g_mem.get());
                        if (km) { km->SpoofInfo(si); fetched = true; } }
        ImGui::Text("Seed    : 0x%08X", si.seed);
        ImGui::Text("Disk    : %.20s", si.diskSerial);
        ImGui::Text("Volume  : %08X", si.volumeSerial);
        ImGui::Text("MAC     : %02X:%02X:%02X:%02X:%02X:%02X",
                    si.mac[0], si.mac[1], si.mac[2], si.mac[3], si.mac[4], si.mac[5]);
        ImGui::TextDisabled("揮発性：ドライバunload/再起動で元に戻ります");
    }
    if (ImGui::CollapsingHeader("Misc")) {
        ImGui::SliderInt("Max FPS", &cfg.maxFps, 30, 300);
        ImGui::TextDisabled("INSERT = メニュー");
    }
    ImGui::End();
}

/* ===================== main ===================== */
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    /* --- バックエンド選択 --- */
    if (cfg.preferDriver) {
        auto km = std::make_unique<KernelMem>();
        if (km->Open()) g_mem = std::move(km);
        else printf("[mem] ドライバなし → RPM にフォールバック\n");
    }
    if (!g_mem) {
        auto hm = std::make_unique<HandleMem>();
        g_mem = std::move(hm);
    }

    if (!g_game.Attach(*g_mem, "")) {
        MessageBoxW(nullptr, L"r5apex.exe をアタッチできませんでした。\n"
                              L"Apex / R5Reloaded を起動してから実行してください。",
                    L"Venice Apex", MB_ICONERROR);
        return 1;
    }

    if (!CreateOverlay()) {
        MessageBoxW(nullptr, L"オーバーレイ作成に失敗しました。", L"Venice Apex", MB_ICONERROR);
        return 1;
    }

    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) goto done;
        }

        if (g_game.IsAlive()) {
            g_game.Update();
            g_game.RunAimbot(cfg.aimbot, cfg.aimOnKey, cfg.aimKey, cfg.aimFov, cfg.aimSmooth);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawESP();
        DrawMenu();
        ImGui::Render();

        const float clear[4] = { 0, 0, 0, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(0, 0);

        Sleep(1000 / std::max(30, cfg.maxFps));
    }

done:
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}