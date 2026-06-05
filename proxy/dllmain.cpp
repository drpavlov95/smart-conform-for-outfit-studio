#include "dialog.h"
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <thread>

static constexpr UINT DRAPE_CMD_ID = 0xD4A9u;

static WNDPROC g_oldProc      = nullptr;
HWND           g_osHwnd        = nullptr;  // OutfitStudio main window
UINT           g_conformAllId  = 0;        // WM_COMMAND ID for "Conform All"

// Toggles the in-memory cloth-aware hook (drape-algo.dll export). Enabled only
// for the duration of a Smart Conform, so the vanilla Conform All stays vanilla.
static std::atomic<PfnDrapeSetHookEnabled> g_setHookEnabled{nullptr};
static std::atomic<PfnDrapeGetCurrentShapes> g_getCurrentShapes{nullptr};
static std::atomic<PfnDrapeClearShapePresets> g_clearShapePresets{nullptr};
static std::atomic<PfnDrapeSetShapePreset> g_setShapePreset{nullptr};
static std::atomic<PfnDrapeSetProcessingIntensity> g_setProcessingIntensity{nullptr};
static std::atomic_bool g_hookReady{false};

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

// ── Progress window (runs on its own thread, safe while OS is blocked) ──
// Creates a small topmost window with a marquee progress bar and a label.
// Owns its own message loop so it stays alive and animating while the main
// thread is blocked in SendMessageA(Conform All). Closed via the done flag.

struct ProgressCtx {
    std::atomic<bool> done{ false };
    HWND              hwnd{ nullptr };
    std::atomic<bool> created{ false };
};

static DWORD WINAPI ProgressThreadProc(LPVOID param) {
    ProgressCtx* ctx = reinterpret_cast<ProgressCtx*>(param);

    // Ensure common controls (progress bar) are available.
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    // Register a simple window class for our progress dialog.
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = "DrapeProgressWnd";
    RegisterClassExA(&wc);   // harmless if already registered

    const int W = 360, H = 100;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hw = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "DrapeProgressWnd", "Smart Conform",
        WS_POPUP | WS_CAPTION,
        (sx - W) / 2, (sy - H) / 2, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) { ctx->created.store(true); return 1; }

    // Line 1
    CreateWindowExA(0, "STATIC", "Processing cloth simulation, please wait...",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 8, W - 20, 18, hw, nullptr, wc.hInstance, nullptr);

    // Line 2
    CreateWindowExA(0, "STATIC", "OutfitStudio might freeze temporarily, don't click anything.",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 28, W - 20, 18, hw, nullptr, wc.hInstance, nullptr);

    // Marquee progress bar
    HWND pb = CreateWindowExA(0, PROGRESS_CLASSA, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
        10, 54, W - 20, 20, hw, nullptr, wc.hInstance, nullptr);
    SendMessageA(pb, PBM_SETMARQUEE, TRUE, 40);  // 40 ms scroll interval

    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);
    ctx->hwnd = hw;
    ctx->created.store(true, std::memory_order_release);

    // Own message loop — keeps the window responsive while OS is frozen.
    MSG msg;
    while (!ctx->done.load(std::memory_order_relaxed)) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(16);  // ~60 fps poll
    }
    DestroyWindow(hw);
    return 0;
}

// RAII wrapper: spawns the progress window on construction, destroys on destruction.
struct ScopedProgressWindow {
    ProgressCtx ctx;
    HANDLE      thread{ nullptr };

    ScopedProgressWindow() {
        thread = CreateThread(nullptr, 0, ProgressThreadProc, &ctx, 0, nullptr);
        // Wait until window is created (or failed) before returning.
        if (thread) while (!ctx.created.load(std::memory_order_acquire)) Sleep(5);
    }
    ~ScopedProgressWindow() {
        ctx.done.store(true, std::memory_order_release);
        if (thread) { WaitForSingleObject(thread, 3000); CloseHandle(thread); }
    }
};

// ── CBT hook: detect when the native "Conform All" dialog is dismissed ──
// The flow is: our menu → first dialog (Smart Conform preset) → SendMessageA
// triggers native Conform All → that opens a SECOND modal dialog → user clicks
// OK → processing starts (main thread freezes). We want the progress window to
// appear only after the user dismisses that second dialog, not during it.
//
// Strategy: install a WH_CBT hook before SendMessageA. Track the FIRST dialog
// window that gets created after the hook is installed. When that window is
// destroyed (user clicked OK/Cancel), fire the progress window.

struct ConformDialogTracker {
    HHOOK               hook{ nullptr };
    HWND                dialogHwnd{ nullptr };   // the native Conform All dialog
    std::atomic<bool>   dialogDismissed{ false };
    ProgressCtx*        progressCtx{ nullptr };
    HANDLE              progressThread{ nullptr };
};

static ConformDialogTracker* g_tracker = nullptr;  // valid only during SendMessageA

static LRESULT CALLBACK ConformCbtProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_tracker) {
        if (nCode == HCBT_CREATEWND) {
            // Capture the first new dialog/popup created after we installed the hook.
            // That's the native "Conform All" confirmation dialog.
            if (!g_tracker->dialogHwnd) {
                HWND hw = reinterpret_cast<HWND>(wParam);
                char cls[64] = {};
                GetClassNameA(hw, cls, sizeof(cls));
                // Only capture real dialogs/popups, not tooltips or combo popups.
                LONG style = GetWindowLongA(hw, GWL_STYLE);
                if ((style & WS_POPUP) || strncmp(cls, "#32770", 6) == 0) {
                    g_tracker->dialogHwnd = hw;
                    DbgLog("CBT: captured conform dialog hwnd=%p class='%s'", (void*)hw, cls);
                }
            }
        } else if (nCode == HCBT_DESTROYWND) {
            // When the captured dialog is destroyed, the user clicked OK/Cancel.
            // Start the progress window NOW — processing is about to begin.
            HWND hw = reinterpret_cast<HWND>(wParam);
            if (hw == g_tracker->dialogHwnd && !g_tracker->dialogDismissed.exchange(true)) {
                DbgLog("CBT: conform dialog dismissed, spawning progress window");
                g_tracker->progressThread = CreateThread(
                    nullptr, 0, ProgressThreadProc, g_tracker->progressCtx, 0, nullptr);
                if (g_tracker->progressThread) {
                    while (!g_tracker->progressCtx->created.load(std::memory_order_acquire))
                        Sleep(5);
                }
            }
        }
    }
    return CallNextHookEx(g_tracker ? g_tracker->hook : nullptr, nCode, wParam, lParam);
}

// ── WndProc subclass: intercept our menu command ──────────────────────

static LRESULT CALLBACK DrapeWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == DRAPE_CMD_ID) {
        DbgLog("Smart Conform All clicked");
        PfnDrapeSetHookEnabled setHookEnabled = g_setHookEnabled.load(std::memory_order_acquire);
        DrapeHookApi api;
        api.setHookEnabled = setHookEnabled;
        api.getCurrentShapes = g_getCurrentShapes.load(std::memory_order_acquire);
        api.clearShapePresets = g_clearShapePresets.load(std::memory_order_acquire);
        api.setShapePreset = g_setShapePreset.load(std::memory_order_acquire);
        api.setProcessingIntensity = g_setProcessingIntensity.load(std::memory_order_acquire);

        if (g_hookReady.load(std::memory_order_acquire) && setHookEnabled &&
            api.getCurrentShapes && api.clearShapePresets && api.setShapePreset &&
            g_conformAllId) {
            if (!ShowDrapePresetDialog(hw, api)) {
                DbgLog("Smart Conform All canceled in preset dialog");
                return 0;
            }

            // Enable the cloth-aware hook ONLY for this conform, run the engine's
            // own Conform All synchronously, then disable it again. The vanilla
            // Conform All menu item is unaffected.
            struct ScopedHookEnable {
                PfnDrapeSetHookEnabled fn;
                explicit ScopedHookEnable(PfnDrapeSetHookEnabled f) : fn(f) { fn(1); }
                ~ScopedHookEnable() { fn(0); }
            } hookScope(setHookEnabled);

            // Install CBT hook to detect when the native Conform All dialog is
            // dismissed (user clicked OK). Progress window spawns at that point,
            // not before — the dialog phase is interactive, not processing.
            ProgressCtx progressCtx;
            ConformDialogTracker tracker;
            tracker.progressCtx = &progressCtx;
            g_tracker = &tracker;
            tracker.hook = SetWindowsHookExA(WH_CBT, ConformCbtProc,
                                              nullptr, GetCurrentThreadId());
            DbgLog("Smart Conform All: CBT hook installed=%p", (void*)tracker.hook);

            SendMessageA(hw, WM_COMMAND, MAKEWPARAM(g_conformAllId, 0), 0);
            // SendMessageA returned — processing done. Tear down.

            if (tracker.hook) { UnhookWindowsHookEx(tracker.hook); tracker.hook = nullptr; }
            g_tracker = nullptr;

            // Close progress window if it was spawned.
            progressCtx.done.store(true, std::memory_order_release);
            if (tracker.progressThread) {
                WaitForSingleObject(tracker.progressThread, 3000);
                CloseHandle(tracker.progressThread);
            }
            DbgLog("Smart Conform All done");
        } else {
            DbgLog("Smart Conform unavailable (ready=%d hook=%p conformAllId=%u)",
                   g_hookReady.load(std::memory_order_relaxed) ? 1 : 0,
                   (void*)setHookEnabled, g_conformAllId);
            MessageBoxA(hw,
                        "Drape hook was not installed for this OutfitStudio build.\n\n"
                        "See %TEMP%\\drape_debug.log for details.",
                        "Drape Smart Conform",
                        MB_OK | MB_ICONWARNING);
        }
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
                          DRAPE_CMD_ID, "Smart Conform All");
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
            auto pSetHookEnabled = reinterpret_cast<PfnDrapeSetHookEnabled>(
                GetProcAddress(hAlgo, "DrapeSetHookEnabled"));
            auto pGetCurrentShapes = reinterpret_cast<PfnDrapeGetCurrentShapes>(
                GetProcAddress(hAlgo, "DrapeGetCurrentShapes"));
            auto pClearShapePresets = reinterpret_cast<PfnDrapeClearShapePresets>(
                GetProcAddress(hAlgo, "DrapeClearShapePresets"));
            auto pSetShapePreset = reinterpret_cast<PfnDrapeSetShapePreset>(
                GetProcAddress(hAlgo, "DrapeSetShapePreset"));
            auto pSetIntensity = reinterpret_cast<PfnDrapeSetProcessingIntensity>(
                GetProcAddress(hAlgo, "DrapeSetProcessingIntensity"));
            if (pInstall) {
                int rc = pInstall(hw, ExeDir().c_str(), 0.5f, 8.0f);
                if (rc == 0 && pSetHookEnabled && pGetCurrentShapes &&
                    pClearShapePresets && pSetShapePreset) {
                    g_setHookEnabled.store(pSetHookEnabled, std::memory_order_release);
                    g_getCurrentShapes.store(pGetCurrentShapes, std::memory_order_release);
                    g_clearShapePresets.store(pClearShapePresets, std::memory_order_release);
                    g_setShapePreset.store(pSetShapePreset, std::memory_order_release);
                    g_setProcessingIntensity.store(pSetIntensity, std::memory_order_release);
                    g_hookReady.store(true, std::memory_order_release);
                } else {
                    g_setHookEnabled.store(nullptr, std::memory_order_release);
                    g_getCurrentShapes.store(nullptr, std::memory_order_release);
                    g_clearShapePresets.store(nullptr, std::memory_order_release);
                    g_setShapePreset.store(nullptr, std::memory_order_release);
                    g_setProcessingIntensity.store(nullptr, std::memory_order_release);
                    g_hookReady.store(false, std::memory_order_release);
                }
                DbgLog("InjectThread: DrapeInstallConformHook rc=%d setEnabled=%p getShapes=%p clearPresets=%p setPreset=%p ready=%d",
                       rc, (void*)pSetHookEnabled, (void*)pGetCurrentShapes,
                       (void*)pClearShapePresets, (void*)pSetShapePreset,
                       g_hookReady.load(std::memory_order_relaxed) ? 1 : 0);
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
