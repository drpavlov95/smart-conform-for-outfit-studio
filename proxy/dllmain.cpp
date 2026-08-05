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
// Redraw request posted to the UI thread. DrawMenuBar() is a SYNCHRONOUS
// cross-thread call when issued from InjectThread: it blocks until the
// window's owning (UI) thread services it. With the conform detour already
// installed, the UI thread can be inside a multi-second conform at that
// moment -> InjectThread and the UI thread wait on each other and
// OutfitStudio freezes on open (reproduced 2026-07-19; the log always ends
// exactly between "InsertMenu" and "DrawMenuBar done"). We post instead, so
// the redraw happens ON the UI thread and the injector never blocks.
static constexpr UINT DRAPE_MSG_REDRAW_MENU = WM_APP + 0x4A9;

static WNDPROC g_oldProc      = nullptr;
HWND           g_osHwnd        = nullptr;  // OutfitStudio main window
UINT           g_conformAllId  = 0;        // WM_COMMAND ID for "Conform All"

// Toggles the in-memory cloth-aware hook (drape-algo.dll export). Enabled only
// for the duration of a Smart Conform, so the vanilla Conform All stays vanilla.
static std::atomic<PfnDrapeSetHookEnabled> g_setHookEnabled{nullptr};
static std::atomic<PfnDrapeGetCurrentShapes> g_getCurrentShapes{nullptr};
static std::atomic<PfnDrapeGetCurrentSliders> g_getCurrentSliders{nullptr};
static std::atomic<PfnDrapeClearShapePresets> g_clearShapePresets{nullptr};
static std::atomic<PfnDrapeSetShapePreset> g_setShapePreset{nullptr};
static std::atomic<PfnDrapeSetProcessingIntensity> g_setProcessingIntensity{nullptr};
static std::atomic<PfnDrapeSetSliderFilter> g_setSliderFilter{nullptr};
static std::atomic_bool g_hookReady{false};
// LAZY HOOK INSTALL (fix 2026-07-19): installing the conform detour from
// InjectThread froze OutfitStudio on open, every time. MinHook SUSPENDS the
// other threads to patch the target; suspending the UI thread during OS
// startup left it never resuming - injection logged SUCCESS while the UI
// thread stopped servicing messages (our posted redraw never ran). The hook
// is not needed until the user actually clicks Smart Conform, so we now
// install it ON the UI thread at first use, where MinHook never has to
// suspend the thread doing the patching.
typedef int (*PfnInstallHook)(void*, const char*, float, float);
static std::atomic<PfnInstallHook> g_installHook{nullptr};
static std::atomic_bool g_hookInstallTried{false};
static std::atomic_bool g_algoLoadTried{false};

static HMODULE          g_selfMod         = nullptr;  // this proxy DLL's module
static DWORD            g_osProcId        = 0;        // OutfitStudio PID (act only when it's focused)
static std::atomic_bool g_conformRunning{false};      // guard: don't re-enter while a conform runs

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

// ── Keyboard shortcut: Ctrl+Alt+C triggers Smart Conform All ──────────
// Third approach. wxWidgets owns its accelerator table (can't join it from an injected
// DLL); a thread-scoped WH_KEYBOARD hook never fired (GL canvas/wx eat the key first);
// a WH_KEYBOARD_LL hook also stayed silent under MO2's usvfs (and usvfs double-loads
// this DLL, chaining two half-configured hooks — see the instance guard in
// InjectThread). So: NO hooks. A dedicated thread polls GetAsyncKeyState at ~33 Hz and
// edge-detects the combo. Immune to wx/GL/usvfs message routing entirely; acts only
// when OutfitStudio is the foreground process, so it is not a global hotkey. Logs every
// detection with all gate values so a silent failure is diagnosable from the log alone.
// NOTE: on ABNT2 (pt-BR) keyboards AltGr == Ctrl+Alt, so AltGr+C also fires (accepted).
static DWORD WINAPI KbPollThreadProc(LPVOID) {
    DbgLog("KbPoll: thread started (Ctrl+Alt+C poller)");
    bool wasDown = false;
    for (;;) {
        Sleep(30);
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;   // VK_MENU = Alt
        const bool cKey = (GetAsyncKeyState('C')        & 0x8000) != 0;
        const bool combo = ctrl && alt && cKey;
        if (combo && !wasDown) {   // fire once per press, not while held
            DWORD fgPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
            const bool ready   = g_hookReady.load(std::memory_order_acquire);
            const bool running = g_conformRunning.load(std::memory_order_acquire);
            DbgLog("KbPoll: Ctrl+Alt+C fgPid=%lu osPid=%lu ready=%d conformAllId=%u running=%d hwnd=%p",
                   fgPid, g_osProcId, ready ? 1 : 0, g_conformAllId, running ? 1 : 0,
                   (void*)g_osHwnd);
            if (fgPid == g_osProcId && ready && g_conformAllId && g_osHwnd && !running)
                PostMessageA(g_osHwnd, WM_COMMAND, MAKEWPARAM(DRAPE_CMD_ID, 0), 0);
        }
        wasDown = combo;
    }
    return 0;
}

// ── WndProc subclass: intercept our menu command ──────────────────────

// LAZY ALGO LOAD (fix 2026-07-19): loading the heavy drape-algo.dll (and
// running all of its static initializers) in the middle of OutfitStudio's
// startup froze the app on open. Nothing here is needed until the user
// actually clicks Smart Conform, so the whole load now happens on the UI
// thread at first use. Startup is left completely free of our heavy code.
static void EnsureAlgoLoaded(HWND hw) {
    if (g_algoLoadTried.exchange(true, std::memory_order_acq_rel)) return;
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
            auto pGetCurrentSliders = reinterpret_cast<PfnDrapeGetCurrentSliders>(
                GetProcAddress(hAlgo, "DrapeGetCurrentSliders"));
            auto pClearShapePresets = reinterpret_cast<PfnDrapeClearShapePresets>(
                GetProcAddress(hAlgo, "DrapeClearShapePresets"));
            auto pSetShapePreset = reinterpret_cast<PfnDrapeSetShapePreset>(
                GetProcAddress(hAlgo, "DrapeSetShapePreset"));
            auto pSetIntensity = reinterpret_cast<PfnDrapeSetProcessingIntensity>(
                GetProcAddress(hAlgo, "DrapeSetProcessingIntensity"));
            auto pSetSliderFilter = reinterpret_cast<PfnDrapeSetSliderFilter>(
                GetProcAddress(hAlgo, "DrapeSetSliderFilter"));
            if (pInstall) {
                // DEFERRED: do not patch anything during startup (see the
                // LAZY HOOK INSTALL note). Just publish the entry points.
                g_installHook.store(pInstall, std::memory_order_release);
                int rc = 0;
                if (rc == 0 && pSetHookEnabled && pGetCurrentShapes &&
                    pGetCurrentSliders && pClearShapePresets && pSetShapePreset &&
                    pSetSliderFilter) {
                    g_setHookEnabled.store(pSetHookEnabled, std::memory_order_release);
                    g_getCurrentShapes.store(pGetCurrentShapes, std::memory_order_release);
                    g_getCurrentSliders.store(pGetCurrentSliders, std::memory_order_release);
                    g_clearShapePresets.store(pClearShapePresets, std::memory_order_release);
                    g_setShapePreset.store(pSetShapePreset, std::memory_order_release);
                    g_setProcessingIntensity.store(pSetIntensity, std::memory_order_release);
                    g_setSliderFilter.store(pSetSliderFilter, std::memory_order_release);
                    g_hookReady.store(true, std::memory_order_release);
                } else {
                    g_setHookEnabled.store(nullptr, std::memory_order_release);
                    g_getCurrentShapes.store(nullptr, std::memory_order_release);
                    g_getCurrentSliders.store(nullptr, std::memory_order_release);
                    g_clearShapePresets.store(nullptr, std::memory_order_release);
                    g_setShapePreset.store(nullptr, std::memory_order_release);
                    g_setProcessingIntensity.store(nullptr, std::memory_order_release);
                    g_setSliderFilter.store(nullptr, std::memory_order_release);
                    g_hookReady.store(false, std::memory_order_release);
                }
                DbgLog("InjectThread: entry points published (hook install DEFERRED to first use) rc=%d setEnabled=%p getShapes=%p getSliders=%p clearPresets=%p setPreset=%p setSliderFilter=%p ready=%d",
                       rc, (void*)pSetHookEnabled, (void*)pGetCurrentShapes,
                       (void*)pGetCurrentSliders, (void*)pClearShapePresets,
                       (void*)pSetShapePreset, (void*)pSetSliderFilter,
                       g_hookReady.load(std::memory_order_relaxed) ? 1 : 0);
            } else {
                DbgLog("InjectThread: DrapeInstallConformHook not found");
            }
        } else {
            DbgLog("InjectThread: failed to load drape-algo.dll for hook");
        }
    }

}

static LRESULT CALLBACK DrapeWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == DRAPE_MSG_REDRAW_MENU) {
        DrawMenuBar(hw);          // safe: we are ON the UI thread here
        DbgLog("DrapeWndProc: menu bar redrawn on UI thread");
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == DRAPE_CMD_ID) {
        // Block re-entry (a second Ctrl+Alt+C during the modal dialog / processing).
        struct RunFlag {
            RunFlag()  { g_conformRunning.store(true,  std::memory_order_release); }
            ~RunFlag() { g_conformRunning.store(false, std::memory_order_release); }
        } runFlag;
        DbgLog("Smart Conform All clicked");
        // Install the detour HERE, on the UI thread, the first time it is
        // needed (see LAZY HOOK INSTALL). MinHook suspends other threads to
        // patch - never the caller - so the UI thread cannot be left frozen.
        EnsureAlgoLoaded(hw);
        if (!g_hookInstallTried.exchange(true, std::memory_order_acq_rel)) {
            PfnInstallHook inst = g_installHook.load(std::memory_order_acquire);
            if (inst) {
                int rc = inst(hw, ExeDir().c_str(), 0.5f, 8.0f);
                DbgLog("Smart Conform All: lazy hook install rc=%d", rc);
                if (rc != 0) g_hookReady.store(false, std::memory_order_release);
            } else {
                DbgLog("Smart Conform All: install entry point missing");
                g_hookReady.store(false, std::memory_order_release);
            }
        }
        PfnDrapeSetHookEnabled setHookEnabled = g_setHookEnabled.load(std::memory_order_acquire);
        DrapeHookApi api;
        api.setHookEnabled = setHookEnabled;
        api.getCurrentShapes = g_getCurrentShapes.load(std::memory_order_acquire);
        api.getCurrentSliders = g_getCurrentSliders.load(std::memory_order_acquire);
        api.clearShapePresets = g_clearShapePresets.load(std::memory_order_acquire);
        api.setShapePreset = g_setShapePreset.load(std::memory_order_acquire);
        api.setProcessingIntensity = g_setProcessingIntensity.load(std::memory_order_acquire);
        api.setSliderFilter = g_setSliderFilter.load(std::memory_order_acquire);

        if (g_hookReady.load(std::memory_order_acquire) && setHookEnabled &&
            api.getCurrentShapes && api.getCurrentSliders &&
            api.clearShapePresets && api.setShapePreset && api.setSliderFilter &&
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

    // Find "Conform All" FIRST — g_conformAllId must be learned even on the
    // "already injected" path below, or a re-entering instance keeps a dead config.
    int nItems = GetMenuItemCount(sliderMenu);
    int insertPos = 1;
    bool foundConformAll = false;
    for (int i = 0; i < nItems; ++i) {
        char lbl[128] = {};
        GetMenuStringA(sliderMenu, i, lbl, sizeof(lbl), MF_BYPOSITION);
        if (std::string(lbl).find("Conform All") != std::string::npos &&
            std::string(lbl).find("Smart") == std::string::npos) {   // skip our own item
            g_conformAllId = GetMenuItemID(sliderMenu, i);
            insertPos = i + 1;
            foundConformAll = true;
            DbgLog("  TryInject: found 'Conform All' at pos %d, id=%u", i, g_conformAllId);
            break;
        }
    }

    // Already injected? (avoid duplicates on retry)
    for (int i = 0; i < nItems; ++i) {
        if (GetMenuItemID(sliderMenu, i) == DRAPE_CMD_ID) {
            DbgLog("  TryInject: Drape item already present (conformAllId=%u)", g_conformAllId);
            return true;
        }
    }
    if (!foundConformAll) {
        DbgLog("  TryInject: 'Conform All' not found; appending at end");
        insertPos = nItems;
    }

    BOOL r1 = InsertMenuA(sliderMenu, insertPos, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    // The "\t" right-aligns the accelerator hint, exactly like the native
    // "Conform All\tCtrl+Shift+C". The key itself is wired via DrapeKeyboardProc.
    BOOL r2 = InsertMenuA(sliderMenu, insertPos + 1, MF_BYPOSITION | MF_STRING,
                          DRAPE_CMD_ID, "Smart Conform All\tCtrl+Alt+C");
    DbgLog("  TryInject: InsertMenu sep=%d item=%d", (int)r1, (int)r2);
    if (!r2) return false;

    // NEVER call DrawMenuBar from this (background) thread - see the
    // DRAPE_MSG_REDRAW_MENU note above. Post it to the UI thread instead.
    PostMessageA(hw, DRAPE_MSG_REDRAW_MENU, 0, 0);
    DbgLog("  TryInject: menu injected, redraw posted to UI thread");
    return true;
}

// ── Injection thread ─────────────────────────────────────────────────

static DWORD WINAPI InjectThread(LPVOID) {
    DbgLog("InjectThread: started");

    // Single-instance guard. Under MO2's usvfs this DLL can be loaded TWICE into the
    // same process (import table + a later LoadLibrary("version.dll") resolving to a
    // different VFS path, which defeats the loader's dedup). The second instance has
    // its own copy of every global: it re-subclasses the window OVER the first
    // instance, sees the menu already injected, and never learns g_conformAllId — so
    // it swallows our WM_COMMAND with a dead config. A named per-PID mutex lets only
    // the FIRST instance install anything; later instances no-op. Handle deliberately
    // leaked (lifetime = process).
    {
        char mtxName[64];
        _snprintf_s(mtxName, sizeof(mtxName), _TRUNCATE,
                    "Local\\DrapeProxyActive_%lu", GetCurrentProcessId());
        HANDLE mtx = CreateMutexA(nullptr, TRUE, mtxName);
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
            DbgLog("InjectThread: another proxy instance is active in this process; standing down");
            CloseHandle(mtx);
            return 0;
        }
        DbgLog("InjectThread: primary proxy instance (mutex '%s')", mtxName);
    }

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

    // Install the Ctrl+Alt+C shortcut via the polling thread (see KbPollThreadProc).
    g_osProcId = GetCurrentProcessId();
    CreateThread(nullptr, 0, KbPollThreadProc, nullptr, 0, nullptr);

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
        g_selfMod = hMod;   // needed as hInstance for the thread-scoped keyboard hook
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        DbgLog("DllMain PROCESS_ATTACH: exe='%s'", exePath);
        // WHICH copy of us is actually running. There are two on disk - one
        // beside OutfitStudio.exe and one in a separate mod folder that a
        // virtual file system may or may not be overlaying - and when they
        // disagree, every other line in this log is about the wrong build.
        // Logging the resolved path settles it without guesswork.
        {
            char selfPath[MAX_PATH] = {};
            GetModuleFileNameA(hMod, selfPath, MAX_PATH);
            DbgLog("DllMain: this version.dll = '%s'", selfPath);
        }

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
