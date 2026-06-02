#include "dialog.h"
#include <windows.h>
#include <string>
#include <algorithm>

static constexpr UINT DRAPE_CMD_ID = 0xD4A9u;

static WNDPROC g_oldProc      = nullptr;
HWND           g_osHwnd        = nullptr;  // OutfitStudio main window
UINT           g_conformAllId  = 0;        // WM_COMMAND ID for "Conform All"

// ── Helpers ──────────────────────────────────────────────────────────

// Returns the directory containing the calling exe (with trailing backslash)
static std::string ExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string s(buf);
    auto p = s.rfind('\\');
    return (p != std::string::npos) ? s.substr(0, p + 1) : s;
}

// Find the OutfitStudio main window (title ends with "Outfit Studio")
static HWND FindOsWindow() {
    struct Ctx { HWND hw; };
    Ctx ctx{};
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        char t[256]; GetWindowTextA(hw, t, sizeof(t));
        std::string title(t);
        if (title.find("Outfit Studio") != std::string::npos) {
            reinterpret_cast<Ctx*>(lp)->hw = hw;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hw;
}

// ── WndProc subclass: intercept our menu command ──────────────────────

static LRESULT CALLBACK DrapeWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == DRAPE_CMD_ID) {
        ShowDrapeDialog(hw, ExeDir());
        return 0;
    }
    return CallWindowProc(g_oldProc, hw, msg, wp, lp);
}

// ── Injection thread ─────────────────────────────────────────────────

static DWORD WINAPI InjectThread(LPVOID) {
    // Poll until OutfitStudio window appears (up to 60 s, more reliable)
    HWND hw = nullptr;
    for (int i = 0; i < 600 && !hw; ++i) {
        Sleep(100);
        hw = FindOsWindow();
    }
    if (!hw) return 1;
    g_osHwnd = hw;

    Sleep(500);  // Give menu bar time to initialize

    HMENU bar = GetMenu(hw);
    if (!bar) return 1;

    // Find "Slider" menu by label
    HMENU sliderMenu = nullptr;
    int nTop = GetMenuItemCount(bar);
    for (int i = 0; i < nTop; ++i) {
        char lbl[64] = {};
        GetMenuStringA(bar, i, lbl, sizeof(lbl), MF_BYPOSITION);
        if (std::string(lbl) == "Slider") {
            sliderMenu = GetSubMenu(bar, i);
            break;
        }
    }
    if (!sliderMenu) return 1;

    // Find "Conform All" position
    int nItems = GetMenuItemCount(sliderMenu);
    int insertPos = 1;
    bool foundConformAll = false;
    for (int i = 0; i < nItems; ++i) {
        char lbl[128] = {};
        GetMenuStringA(sliderMenu, i, lbl, sizeof(lbl), MF_BYPOSITION);
        if (std::string(lbl).find("Conform All") != std::string::npos) {
            g_conformAllId = GetMenuItemID(sliderMenu, i);
            insertPos = i + 1;
            foundConformAll = true;
            break;
        }
    }
    if (!foundConformAll) return 1;

    // Insert separator + "Drape Conform..." right after Conform All
    InsertMenuA(sliderMenu, insertPos,     MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuA(sliderMenu, insertPos + 1, MF_BYPOSITION | MF_STRING,
                DRAPE_CMD_ID, "Drape Conform...");
    DrawMenuBar(hw);

    // Subclass the window to catch WM_COMMAND for our item
    g_oldProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hw, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DrapeWndProc)));
    return 0;
}

// ── DllMain ──────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Only activate inside OutfitStudio — not BodySlide or other tools
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exe(exePath);
        std::transform(exe.begin(), exe.end(), exe.begin(), ::tolower);
        if (exe.find("outfitstudio") == std::string::npos)
            return TRUE;  // safe no-op in BodySlide.exe / BodySlide x64.exe

        DisableThreadLibraryCalls(hMod);
        CreateThread(nullptr, 0, InjectThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
