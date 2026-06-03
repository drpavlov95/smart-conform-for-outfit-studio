#include "dialog.h"
#include <windows.h>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdarg>

static constexpr UINT DRAPE_CMD_ID = 0xD4A9u;

static WNDPROC g_oldProc      = nullptr;
HWND           g_osHwnd        = nullptr;  // OutfitStudio main window
UINT           g_conformAllId  = 0;        // WM_COMMAND ID for "Conform All"

// ── Debug logging (pure Win32, safe in DllMain context) ───────────────
// Writes to %TEMP%\drape_debug.log so it works regardless of MO2 VFS.

static void DbgLog(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
    va_end(args);

    // Timestamp
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%02d:%02d:%02d.%03d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    char path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, path);
    if (len == 0 || len > MAX_PATH - 20) return;
    strcat_s(path, MAX_PATH, "drape_debug.log");

    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD written;
    WriteFile(h, line, (DWORD)n, &written, nullptr);
    CloseHandle(h);
}

// ── Helpers ──────────────────────────────────────────────────────────

// Returns the directory containing the calling exe (with trailing backslash)
static std::string ExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string s(buf);
    auto p = s.rfind('\\');
    return (p != std::string::npos) ? s.substr(0, p + 1) : s;
}

// Find the OutfitStudio main frame: top-level, visible, has a menu bar,
// title contains "Outfit Studio". Filters out splash/tooltip windows.
static HWND FindOsWindow() {
    struct Ctx { HWND hw; };
    Ctx ctx{};
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hw)) return TRUE;
        char t[256] = {};
        GetWindowTextA(hw, t, sizeof(t));
        std::string title(t);
        if (title.find("Outfit Studio") == std::string::npos) return TRUE;
        // Must have a menu bar (the real frame, not a child/tooltip)
        if (GetMenu(hw) == nullptr) return TRUE;
        reinterpret_cast<Ctx*>(lp)->hw = hw;
        return FALSE;  // found it
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hw;
}

// ── WndProc subclass: intercept our menu command ──────────────────────

static LRESULT CALLBACK DrapeWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == DRAPE_CMD_ID) {
        DbgLog("WM_COMMAND received for Drape menu item");
        ShowDrapeDialog(hw, ExeDir());
        return 0;
    }
    return CallWindowProc(g_oldProc, hw, msg, wp, lp);
}

// ── Try to inject the menu item. Returns true on success. ─────────────

static bool TryInjectMenu(HWND hw) {
    HMENU bar = GetMenu(hw);
    if (!bar) { DbgLog("  TryInject: GetMenu returned NULL"); return false; }

    int nTop = GetMenuItemCount(bar);
    DbgLog("  TryInject: menu bar has %d top-level items", nTop);

    // Find "Slider" menu by label
    HMENU sliderMenu = nullptr;
    for (int i = 0; i < nTop; ++i) {
        char lbl[64] = {};
        GetMenuStringA(bar, i, lbl, sizeof(lbl), MF_BYPOSITION);
        DbgLog("    top menu[%d] = '%s'", i, lbl);
        if (std::string(lbl).find("Slider") != std::string::npos) {
            sliderMenu = GetSubMenu(bar, i);
        }
    }
    if (!sliderMenu) { DbgLog("  TryInject: 'Slider' menu not found"); return false; }

    // Already injected? (avoid duplicates on retry)
    int nItems = GetMenuItemCount(sliderMenu);
    for (int i = 0; i < nItems; ++i) {
        if (GetMenuItemID(sliderMenu, i) == DRAPE_CMD_ID) {
            DbgLog("  TryInject: Drape item already present");
            return true;
        }
    }

    // Find "Conform All" position
    int insertPos = 1;
    bool foundConformAll = false;
    for (int i = 0; i < nItems; ++i) {
        char lbl[128] = {};
        GetMenuStringA(sliderMenu, i, lbl, sizeof(lbl), MF_BYPOSITION);
        if (std::string(lbl).find("Conform All") != std::string::npos) {
            g_conformAllId = GetMenuItemID(sliderMenu, i);
            insertPos = i + 1;
            foundConformAll = true;
            DbgLog("  TryInject: found 'Conform All' at pos %d, id=%u", i, g_conformAllId);
            break;
        }
    }
    if (!foundConformAll) {
        DbgLog("  TryInject: 'Conform All' not found; appending at end");
        insertPos = nItems;
    }

    BOOL r1 = InsertMenuA(sliderMenu, insertPos, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    BOOL r2 = InsertMenuA(sliderMenu, insertPos + 1, MF_BYPOSITION | MF_STRING,
                          DRAPE_CMD_ID, "Smart Conform...");
    DbgLog("  TryInject: InsertMenu sep=%d item=%d", (int)r1, (int)r2);
    if (!r2) return false;

    DrawMenuBar(hw);
    DbgLog("  TryInject: menu injected + DrawMenuBar done");
    return true;
}

// ── Injection thread ─────────────────────────────────────────────────

static DWORD WINAPI InjectThread(LPVOID) {
    DbgLog("InjectThread: started");

    // Poll until OutfitStudio frame appears (up to 60 s)
    HWND hw = nullptr;
    for (int i = 0; i < 600 && !hw; ++i) {
        Sleep(100);
        hw = FindOsWindow();
    }
    if (!hw) { DbgLog("InjectThread: window NEVER found (timeout)"); return 1; }
    g_osHwnd = hw;
    DbgLog("InjectThread: found OS window hwnd=0x%p", (void*)hw);

    // Subclass FIRST so we catch the command even if injection retries
    g_oldProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hw, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DrapeWndProc)));
    DbgLog("InjectThread: subclassed WndProc, oldProc=0x%p", (void*)g_oldProc);

    // Install the in-memory "Smart Conform" hook: load the heavy DLL (its nifly
    // static initializers run now, after OutfitStudio is up, so no startup crash)
    // and let it hook Automorph::GenerateResultDiff.
    {
        std::string algoPath = ExeDir() + "drape-algo.dll";
        HMODULE hAlgo = LoadLibraryA(algoPath.c_str());
        if (hAlgo) {
            typedef int (*PfnInstall)(void*, const char*, float, float);
            auto pInstall = reinterpret_cast<PfnInstall>(
                GetProcAddress(hAlgo, "DrapeInstallConformHook"));
            if (pInstall) {
                int rc = pInstall(hw, ExeDir().c_str(), 2.0f, 20.0f);
                DbgLog("InjectThread: DrapeInstallConformHook rc=%d", rc);
            } else {
                DbgLog("InjectThread: DrapeInstallConformHook not found");
            }
        } else {
            DbgLog("InjectThread: failed to load drape-algo.dll for hook");
        }
    }

    // Retry injection up to 20 times (menu bar may not be ready immediately)
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (TryInjectMenu(hw)) {
            DbgLog("InjectThread: SUCCESS on attempt %d", attempt);
            return 0;
        }
        Sleep(500);
    }
    DbgLog("InjectThread: FAILED after 20 attempts");
    return 1;
}

// ── DllMain ──────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        DbgLog("DllMain PROCESS_ATTACH: exe='%s'", exePath);

        std::string exe(exePath);
        std::transform(exe.begin(), exe.end(), exe.begin(), ::tolower);
        if (exe.find("outfitstudio") == std::string::npos) {
            DbgLog("DllMain: not OutfitStudio, skipping injection");
            return TRUE;  // safe no-op in BodySlide.exe / BodySlide x64.exe
        }

        DbgLog("DllMain: OutfitStudio detected, starting InjectThread");
        DisableThreadLibraryCalls(hMod);
        CreateThread(nullptr, 0, InjectThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
