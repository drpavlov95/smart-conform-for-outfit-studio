#include "dialog.h"
#include "../proxy/drape_iface.h"

#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <set>
#include <windows.h>

#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;

extern HWND g_osHwnd;
extern UINT g_conformAllId;

// ── Control IDs ──────────────────────────────────────────────────────
enum : UINT {
    IDC_OSP_EDIT    = 101,
    IDC_OSP_BROWSE  = 102,
    IDC_SHAPES_LIST = 103,
    IDC_DETECT_BTN  = 104,
    IDC_CONFORM_BTN = 105,
    IDC_CANCEL_BTN  = 106,
    IDC_PROGRESS    = 107,
    IDC_STATUS      = 108,
    IDC_D_TIGHT_EDT = 109,
    IDC_D_RIGID_EDT = 110,
    COL_SHAPE = 0, COL_ZONE = 1, COL_VERTS = 2,
};

#define WM_DRAPE_DONE  (WM_USER + 1)
#define WM_DRAPE_LINE  (WM_USER + 2)
#define WM_DRAPE_ZONE  (WM_USER + 3)

// ── SliderSet info (mirrors algo.cpp) ─────────────────────────────────
struct SetInfo {
    std::string name;
    std::string dataFolder;
    std::string sourceFile;
    std::string refBodyShape;
    std::set<std::string> osdFiles;
};

struct DlgData {
    std::string bsDir;
    std::string ospPath;
    std::vector<SetInfo> ospSets;
    std::vector<int> selectedSets;   // indices of checked items
    HWND lvShapes  = nullptr;
    HWND edStatus  = nullptr;
    HWND progress  = nullptr;
    HWND btnDetect = nullptr;
    HWND btnConform= nullptr;
    std::atomic<bool> running{false};

    // drape-algo API (loaded on first use)
    HMODULE hAlgo = nullptr;
    DrapeAlgoApi api{};
};

// ── OSP parsing ───────────────────────────────────────────────────────

static std::string xmlText(const std::string& line,
                            const char* open, const char* close) {
    auto p = line.find(open);
    if (p == std::string::npos) return "";
    p += strlen(open);
    auto q = line.find(close, p);
    return (q != std::string::npos) ? line.substr(p, q - p) : "";
}

static bool isBodyShape(const std::string& t) {
    static const char* K[] = {
        "CBBE", "HIMBO - Body HP", "BHUNP Body", "UNP Body",
        "UUNP Body", "3BA", "TBD Body", "SAM Body", "CBBE Body",
        "3BBB Body", "Bra", "Panty", nullptr
    };
    for (int i = 0; K[i]; ++i)
        if (t == K[i]) return true;
    return false;
}

static std::vector<SetInfo> ParseOspSets(const std::string& ospPath) {
    std::vector<SetInfo> sets;
    std::ifstream f(ospPath);
    std::string line;
    SetInfo cur;
    bool inSet = false;

    while (std::getline(f, line)) {
        if (!inSet) {
            auto p = line.find("<SliderSet name=\"");
            if (p != std::string::npos) {
                p += 17;
                auto q = line.find('"', p);
                if (q != std::string::npos) {
                    cur.name = line.substr(p, q - p);
                    inSet = true;
                }
            }
        } else {
            if (cur.dataFolder.empty())
                cur.dataFolder = xmlText(line, "<DataFolder>", "</DataFolder>");
            if (cur.sourceFile.empty())
                cur.sourceFile = xmlText(line, "<SourceFile>", "</SourceFile>");

            if (cur.refBodyShape.empty()) {
                auto p = line.find("<Shape target=\"");
                if (p != std::string::npos) {
                    p += 15;
                    auto q = line.find('"', p);
                    if (q != std::string::npos) {
                        std::string target = line.substr(p, q - p);
                        if (isBodyShape(target)) cur.refBodyShape = target;
                    }
                }
            }

            if (line.find("local=\"true\"") != std::string::npos) {
                auto p = line.find('>');
                if (p != std::string::npos) {
                    auto q = line.find('<', ++p);
                    if (q != std::string::npos) {
                        std::string val = line.substr(p, q - p);
                        auto bsl = val.find('\\');
                        if (bsl != std::string::npos)
                            cur.osdFiles.insert(val.substr(0, bsl));
                    }
                }
            }

            if (line.find("</SliderSet>") != std::string::npos) {
                sets.push_back(cur);
                cur = {}; inSet = false;
            }
        }
    }
    return sets;
}

// ── Debug log (shared file with the proxy injector) ───────────────────

static void DbgLog(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%02d:%02d:%02d.%03d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    char path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, path);
    if (!len || len > MAX_PATH - 20) return;
    strcat_s(path, MAX_PATH, "drape_debug.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD w; WriteFile(h, line, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

// ── Window helpers ────────────────────────────────────────────────────

static std::string toLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string ProjectFromTitle(HWND osHwnd) {
    char buf[512] = {}; GetWindowTextA(osHwnd, buf, sizeof(buf));
    std::string t(buf);
    DbgLog("dialog: OS window title = '%s'", t.c_str());
    const std::string suffix = " - Outfit Studio";
    auto p = t.rfind(suffix);
    if (p == std::string::npos) return "";
    std::string name = t.substr(0, p);
    if (!name.empty() && name.back() == '*') name.pop_back();
    return name;
}

// Resolve the open project's .osp by: (1) SliderSet name attribute (exact),
// (2) .osp filename stem (exact), (3) partial contains — all case-insensitive.
static std::string FindOsp(const std::string& bsDir, const std::string& proj) {
    fs::path ssDir = fs::path(bsDir) / "SliderSets";
    if (!fs::is_directory(ssDir)) {
        DbgLog("dialog: SliderSets dir not found at '%s'", ssDir.string().c_str());
        return "";
    }
    std::string projL = toLower(proj);
    std::string fallback;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(ssDir, ec)) {
        if (e.path().extension() != ".osp") continue;
        if (toLower(e.path().stem().string()) == projL)
            return e.path().string();
        std::ifstream f(e.path()); std::string line;
        while (std::getline(f, line)) {
            auto p = line.find("<SliderSet name=\"");
            if (p == std::string::npos) continue;
            p += 17;
            auto q = line.find('"', p);
            if (q == std::string::npos) continue;
            std::string setL = toLower(line.substr(p, q - p));
            if (setL == projL) return e.path().string();
            if (fallback.empty() && !projL.empty() &&
                (setL.find(projL) != std::string::npos ||
                 projL.find(setL) != std::string::npos))
                fallback = e.path().string();
        }
    }
    if (!fallback.empty()) DbgLog("dialog: osp partial-match fallback used");
    return fallback;
}

static void PopulateSets(HWND lv, const std::vector<SetInfo>& sets) {
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)sets.size(); ++i) {
        LVITEMA item{};
        item.mask = LVIF_TEXT | LVIF_STATE;
        item.iItem = i;
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state = INDEXTOSTATEIMAGEMASK(2);
        item.pszText = const_cast<char*>(sets[i].name.c_str());
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, COL_ZONE,  const_cast<char*>("—"));
        ListView_SetItemText(lv, i, COL_VERTS, const_cast<char*>("—"));
    }
}

static void PostLog(HWND dlg, const std::string& msg) {
    PostMessageA(dlg, WM_DRAPE_LINE, 0, (LPARAM)_strdup(msg.c_str()));
}

// ── Load drape-algo.dll ────────────────────────────────────────────────

static bool LoadDrapeAlgo(DlgData* d) {
    if (d->hAlgo) return true;  // Already loaded

    char exeDir[MAX_PATH];
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    std::string algoPath = fs::path(exeDir).parent_path().string() + "\\drape-algo.dll";

    d->hAlgo = LoadLibraryA(algoPath.c_str());
    if (!d->hAlgo) {
        PostLog(nullptr, std::string("ERROR: Cannot load drape-algo.dll from ") + algoPath);
        return false;
    }

    typedef int (*PfnGetApi)(DrapeAlgoApi*);
    PfnGetApi pGetApi = (PfnGetApi)GetProcAddress(d->hAlgo, "DrapeGetApi");
    if (!pGetApi || pGetApi(&d->api) != 0) {
        PostLog(nullptr, "ERROR: DrapeGetApi() failed");
        FreeLibrary(d->hAlgo);
        d->hAlgo = nullptr;
        return false;
    }

    return true;
}

// ── Callbacks from drape-algo (posted as WM_DRAPE_* messages) ──────────

static void CALLBACK LogCallback(void* ctx, const char* line) {
    HWND dlg = (HWND)ctx;
    if (dlg) PostMessageA(dlg, WM_DRAPE_LINE, 0, (LPARAM)_strdup(line));
}

static void CALLBACK ZoneCallback(void* ctx, int setIndex, const char* zone,
                                   int tight, int loose, int rigid, int total) {
    HWND dlg = (HWND)ctx;
    if (dlg) {
        std::string payload = std::string(zone) + "|" + std::to_string(total);
        PostMessageA(dlg, WM_DRAPE_ZONE, (WPARAM)setIndex, (LPARAM)_strdup(payload.c_str()));
    }
}

// ── Detect via drape-algo ──────────────────────────────────────────────

static void RunDetect(HWND dlg, DlgData* d, float dTight, float dRigid) {
    if (!LoadDrapeAlgo(d) || !d->api.detect) {
        PostLog(dlg, "ERROR: drape-algo.dll not available");
        PostMessageA(dlg, WM_DRAPE_DONE, 1, 0);
        return;
    }

    int n = d->selectedSets.size();
    if (n == 0) {
        PostLog(dlg, "No sets selected");
        PostMessageA(dlg, WM_DRAPE_DONE, 1, 0);
        return;
    }

    int result = d->api.detect(
        d->bsDir.c_str(),
        d->ospPath.c_str(),
        d->selectedSets.data(),
        (int)d->selectedSets.size(),
        dTight, dRigid,
        LogCallback, dlg,
        ZoneCallback, dlg
    );

    PostMessageA(dlg, WM_DRAPE_DONE, result >= 0 ? 0 : 1, 0);
}

// ── Conform via drape-algo ─────────────────────────────────────────────

static void RunConform(HWND dlg, DlgData* d, float dTight, float dRigid) {
    if (!LoadDrapeAlgo(d) || !d->api.conform) {
        PostLog(dlg, "ERROR: drape-algo.dll not available");
        PostMessageA(dlg, WM_DRAPE_DONE, 1, 0);
        return;
    }

    if (g_conformAllId == 0) {
        PostLog(dlg, "ERROR: Conform All not found in OutfitStudio menu.");
        PostMessageA(dlg, WM_DRAPE_DONE, 1, 0);
        return;
    }

    int n = d->selectedSets.size();
    if (n == 0) {
        PostLog(dlg, "No sets selected");
        PostMessageA(dlg, WM_DRAPE_DONE, 1, 0);
        return;
    }

    int result = d->api.conform(
        g_osHwnd,
        g_conformAllId,
        d->bsDir.c_str(),
        d->ospPath.c_str(),
        d->selectedSets.data(),
        (int)d->selectedSets.size(),
        dTight, dRigid,
        LogCallback, dlg
    );

    PostMessageA(dlg, WM_DRAPE_DONE, result >= 0 ? 0 : 1, 0);
}

// ── Dialog procedure ──────────────────────────────────────────────────

static INT_PTR CALLBACK DrapeDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    DlgData* d = reinterpret_cast<DlgData*>(GetWindowLongPtrA(dlg, GWLP_USERDATA));

    switch (msg) {

    case WM_INITDIALOG: {
        d = reinterpret_cast<DlgData*>(lp);
        SetWindowLongPtrA(dlg, GWLP_USERDATA, lp);
        SetWindowTextA(dlg, "Smart Conform");

        HWND lv = GetDlgItem(dlg, IDC_SHAPES_LIST);
        d->lvShapes  = lv;
        d->edStatus  = GetDlgItem(dlg, IDC_STATUS);
        d->progress  = GetDlgItem(dlg, IDC_PROGRESS);
        d->btnDetect = GetDlgItem(dlg, IDC_DETECT_BTN);
        d->btnConform= GetDlgItem(dlg, IDC_CONFORM_BTN);

        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        auto addCol = [&](int i, const char* text, int w) {
            LVCOLUMNA c{LVCF_TEXT | LVCF_WIDTH, 0, w, const_cast<char*>(text)};
            ListView_InsertColumn(lv, i, &c);
        };
        RECT r; GetClientRect(lv, &r);
        int total = r.right - r.left;
        addCol(COL_SHAPE, "Shape", total - 140);
        addCol(COL_ZONE,  "Zone",  80);
        addCol(COL_VERTS, "Verts", 60);

        SetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, "2.0");
        SetDlgItemTextA(dlg, IDC_D_RIGID_EDT, "20.0");
        SendMessageA(d->progress, PBM_SETRANGE32, 0, 100);

        // Auto-load the project currently open in OutfitStudio (read from its
        // window title). g_osHwnd is the frame we injected into — more reliable
        // than GetParent(dlg).
        HWND osWnd = g_osHwnd ? g_osHwnd : GetParent(dlg);
        std::string proj = ProjectFromTitle(osWnd);
        DbgLog("dialog: parsed project name = '%s'  bsDir='%s'",
               proj.c_str(), d->bsDir.c_str());
        if (!proj.empty()) {
            SetDlgItemTextA(dlg, IDC_OSP_EDIT, proj.c_str());
            std::string osp = FindOsp(d->bsDir, proj);
            DbgLog("dialog: resolved .osp = '%s'", osp.c_str());
            if (!osp.empty()) {
                d->ospPath = osp;
                d->ospSets = ParseOspSets(osp);
                DbgLog("dialog: parsed %d slider set(s)", (int)d->ospSets.size());
                PopulateSets(lv, d->ospSets);
            } else {
                PostLog(dlg, "Could not auto-find the .osp for \"" + proj +
                             "\". Use Browse to select it, then run.");
            }
        } else {
            PostLog(dlg, "No open project detected from the window title. "
                         "Save your project in OutfitStudio first, then Browse.");
        }
        return TRUE;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wp);

        if (id == IDC_OSP_BROWSE) {
            char path[MAX_PATH] = "";
            OPENFILENAMEA ofn{sizeof(ofn)};
            ofn.hwndOwner   = dlg;
            ofn.lpstrFilter = "Outfit Studio Project\0*.osp\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                d->ospPath = path;
                SetDlgItemTextA(dlg, IDC_OSP_EDIT,
                    fs::path(path).stem().string().c_str());
                d->ospSets = ParseOspSets(path);
                PopulateSets(d->lvShapes, d->ospSets);
            }
        }

        else if (id == IDC_DETECT_BTN && !d->running) {
            if (d->ospSets.empty()) {
                MessageBoxA(dlg, "Load a project first.", "Drape", MB_OK);
                break;
            }

            // Collect checked items
            d->selectedSets.clear();
            int n = ListView_GetItemCount(d->lvShapes);
            for (int i = 0; i < n; ++i)
                if (ListView_GetCheckState(d->lvShapes, i))
                    d->selectedSets.push_back(i);

            if (d->selectedSets.empty()) {
                MessageBoxA(dlg, "Select at least one SliderSet.", "Drape", MB_OK);
                break;
            }

            char tight[16], rigid[16];
            GetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, tight, sizeof(tight));
            GetDlgItemTextA(dlg, IDC_D_RIGID_EDT, rigid, sizeof(rigid));
            d->running = true;
            EnableWindow(d->btnDetect, FALSE);
            EnableWindow(d->btnConform, FALSE);
            SetWindowTextA(d->edStatus, "");
            SendMessageA(d->progress, PBM_SETPOS, 0, 0);
            float dT = (float)atof(tight), dR = (float)atof(rigid);
            std::thread([=]{ RunDetect(dlg, d, dT, dR); }).detach();
        }

        else if (id == IDC_CONFORM_BTN && !d->running) {
            if (d->ospSets.empty()) {
                MessageBoxA(dlg, "Load a project first.", "Drape", MB_OK);
                break;
            }

            // Collect checked items
            d->selectedSets.clear();
            int n = ListView_GetItemCount(d->lvShapes);
            for (int i = 0; i < n; ++i)
                if (ListView_GetCheckState(d->lvShapes, i))
                    d->selectedSets.push_back(i);

            if (d->selectedSets.empty()) {
                MessageBoxA(dlg, "Select at least one SliderSet.", "Drape", MB_OK);
                break;
            }

            char tight[16], rigid[16];
            GetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, tight, sizeof(tight));
            GetDlgItemTextA(dlg, IDC_D_RIGID_EDT, rigid, sizeof(rigid));
            d->running = true;
            EnableWindow(d->btnDetect, FALSE);
            EnableWindow(d->btnConform, FALSE);
            SetWindowTextA(d->edStatus, "");
            SendMessageA(d->progress, PBM_SETPOS, 0, 0);
            float dT = (float)atof(tight), dR = (float)atof(rigid);
            std::thread([=]{ RunConform(dlg, d, dT, dR); }).detach();
        }

        else if (id == IDC_CANCEL_BTN || id == IDCANCEL) {
            if (!d->running) EndDialog(dlg, 0);
        }
        break;
    }

    case WM_DRAPE_LINE: {
        char* line = reinterpret_cast<char*>(lp);
        if (line && d->edStatus) {
            int len = GetWindowTextLengthA(d->edStatus);
            SendMessageA(d->edStatus, EM_SETSEL, len, len);
            std::string s(line); s += "\r\n";
            SendMessageA(d->edStatus, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
            free(line);
        }
        break;
    }

    case WM_DRAPE_ZONE: {
        int item = (int)wp;
        char* payload = reinterpret_cast<char*>(lp);
        if (payload && d->lvShapes && item >= 0 && item < ListView_GetItemCount(d->lvShapes)) {
            std::istringstream ss(payload);
            std::string zone, verts;
            std::getline(ss, zone, '|');
            std::getline(ss, verts, '|');
            ListView_SetItemText(d->lvShapes, item, COL_ZONE,  const_cast<char*>(zone.c_str()));
            ListView_SetItemText(d->lvShapes, item, COL_VERTS, const_cast<char*>(verts.c_str()));
            free(payload);
        }
        break;
    }

    case WM_DRAPE_DONE: {
        if (d) {
            d->running = false;
            EnableWindow(d->btnDetect, TRUE);
            EnableWindow(d->btnConform, TRUE);
            SendMessageA(d->progress, PBM_SETPOS, (DWORD)wp == 0 ? 100 : 0, 0);
        }
        break;
    }

    case WM_SIZE: {
        if (!d) break;
        RECT cr; GetClientRect(dlg, &cr);
        int w = cr.right - 16, top = 32;
        SetWindowPos(GetDlgItem(dlg, IDC_SHAPES_LIST), nullptr,
                     8, top, w, cr.bottom - top - 175, SWP_NOZORDER);
        int statusTop = cr.bottom - 159;
        SetWindowPos(d->edStatus, nullptr, 8, statusTop, w, 110, SWP_NOZORDER);
        int btnY = cr.bottom - 40;
        SetWindowPos(d->btnDetect,  nullptr, 8,      btnY, 120, 28, SWP_NOZORDER);
        SetWindowPos(d->btnConform, nullptr, 136,    btnY, 150, 28, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(dlg, IDC_CANCEL_BTN), nullptr, w - 84, btnY, 84, 28, SWP_NOZORDER);
        break;
    }

    } // switch
    return FALSE;
}

// ── Build dialog template ─────────────────────────────────────────────

struct DlgBuilder {
    std::vector<WORD> data;
    void w(WORD v)  { data.push_back(v); }
    void dw(DWORD v){ w(LOWORD(v)); w(HIWORD(v)); }
    void str(const wchar_t* s) { for (; *s; ++s) w((WORD)*s); w(0); }
    void align() { if (data.size() & 1) w(0); }
    void control(DWORD style, short x, short y, short cx, short cy,
                 WORD id, const wchar_t* cls, const wchar_t* text) {
        align();
        dw(style | WS_CHILD | WS_VISIBLE); dw(0);
        w(x); w(y); w(cx); w(cy);
        w(id); str(cls); str(text); w(0);
    }
};

enum : UINT {
    IDC_PRESET_LIST    = 2001,
    IDC_EDIT_COMBO     = 2002,  // inline dropdown over the Behavior cell
    IDC_PRESET_STATUS  = 2003,
    IDC_INT_LOW        = 2004,
    IDC_INT_MED        = 2005,
    IDC_INT_HIGH       = 2006,
    IDC_AUTO_DETECT    = 2007,
    IDC_SUBTITLE       = 2009,
    IDC_GRP_MESHES     = 2010,
    IDC_GRP_QUALITY    = 2011,
    IDC_QUALITY_DESC   = 2013,
    COL_PRESET_SHAPE   = 0,
    COL_PRESET_VALUE   = 1,
};

struct PresetRow {
    std::string shape;
    int preset = DRAPE_PRESET_AUTO;
};

struct PresetDlgData {
    DrapeHookApi api{};
    HWND lv     = nullptr;
    HWND combo  = nullptr;  // inline edit combo (shown over a Behavior cell)
    HWND status = nullptr;
    int  intensity = 1;     // 0=Low 1=Medium 2=High
    int  editRow   = -1;    // row currently being edited inline, or -1
    std::vector<PresetRow> rows;
};

// Returns the human-readable description for a quality level.
static const char* QualityDesc(int level) {
    switch (level) {
    case 0:  return "Fewer passes - fastest, good for a quick preview.";
    case 2:  return "Extra passes - slowest, best for extreme body morphs.";
    default: return "Balanced quality and speed (recommended).";
    }
}

static const char* PresetLabel(int preset) {
    switch (preset) {
    case DRAPE_PRESET_SKIN_TIGHT: return "Skin-tight";
    case DRAPE_PRESET_FITTED_CLOTH: return "Fitted Cloth";
    case DRAPE_PRESET_LOOSE_DRAPED: return "Loose Draped";
    case DRAPE_PRESET_TUBULAR: return "Tubular";
    case DRAPE_PRESET_FLEXIBLE_ARMOR: return "Flexible Armor";
    case DRAPE_PRESET_RIGID_ARMOR: return "Rigid Armor";
    case DRAPE_PRESET_BELT: return "Belt / Waist Item";
    case DRAPE_PRESET_RIGID_ACCESSORY: return "Rigid Accessory";
    default: return "Auto";
    }
}

static int PresetFromComboIndex(int idx) {
    static const int kPresets[] = {
        DRAPE_PRESET_AUTO,
        DRAPE_PRESET_SKIN_TIGHT,
        DRAPE_PRESET_FITTED_CLOTH,
        DRAPE_PRESET_LOOSE_DRAPED,
        DRAPE_PRESET_TUBULAR,
        DRAPE_PRESET_FLEXIBLE_ARMOR,
        DRAPE_PRESET_RIGID_ARMOR,
        DRAPE_PRESET_BELT,
        DRAPE_PRESET_RIGID_ACCESSORY,
    };
    if (idx < 0 || idx >= (int)std::size(kPresets))
        return DRAPE_PRESET_AUTO;
    return kPresets[idx];
}

static int ComboIndexForPreset(int preset) {
    for (int i = 0; i < 9; ++i)
        if (PresetFromComboIndex(i) == preset)
            return i;
    return 0;
}

static bool NameContains(const std::string& lowerName, const char* token) {
    return lowerName.find(token) != std::string::npos;
}

static int GuessPresetForShapeUi(const std::string& shape) {
    std::string n = toLower(shape);
    if (NameContains(n, "belt") || NameContains(n, "waist") ||
        NameContains(n, "girdle") || NameContains(n, "corset"))
        return DRAPE_PRESET_BELT;
    if (NameContains(n, "buckle") || NameContains(n, "medallion") ||
        NameContains(n, "pendant") || NameContains(n, "amulet") ||
        NameContains(n, "pouch") || NameContains(n, "bag") ||
        NameContains(n, "deco") || NameContains(n, "dec"))
        return DRAPE_PRESET_RIGID_ACCESSORY;
    if (NameContains(n, "skirt") || NameContains(n, "dress") ||
        NameContains(n, "robe") || NameContains(n, "tunic") ||
        NameContains(n, "fur") || NameContains(n, "cloak"))
        return DRAPE_PRESET_LOOSE_DRAPED;
    if (NameContains(n, "pants") || NameContains(n, "sleeve") ||
        NameContains(n, "glove") || NameContains(n, "boot") ||
        NameContains(n, "arm") || NameContains(n, "leg"))
        return DRAPE_PRESET_TUBULAR;
    if (NameContains(n, "pauldron") || NameContains(n, "shoulder") ||
        NameContains(n, "plate"))
        return DRAPE_PRESET_RIGID_ARMOR;
    if (NameContains(n, "mail") || NameContains(n, "leather"))
        return DRAPE_PRESET_FLEXIBLE_ARMOR;
    if (NameContains(n, "cloth"))
        return DRAPE_PRESET_FITTED_CLOTH;
    return DRAPE_PRESET_AUTO;
}

static void PopulatePresetCombo(HWND combo) {
    for (int i = 0; i < 9; ++i) {
        int preset = PresetFromComboIndex(i);
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)PresetLabel(preset));
    }
}

static void PopulatePresetRows(HWND lv, std::vector<PresetRow>& rows) {
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)rows.size(); ++i) {
        LVITEMA item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<char*>(rows[i].shape.c_str());
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, COL_PRESET_VALUE,
                             const_cast<char*>(PresetLabel(rows[i].preset)));
    }
}

static int SelectedPresetRow(HWND lv) {
    return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

static void UpdatePresetStatus(PresetDlgData* d) {
    if (!d || !d->status) return;
    size_t n = d->rows.size();
    std::string s = std::to_string(n) + (n == 1 ? " mesh ready." : " meshes ready.");
    s += "  Click a Behavior cell to change it.";
    SetWindowTextA(d->status, s.c_str());
}

// Commit the inline edit combo back to the row, then hide it.
static void CommitInlineEdit(PresetDlgData* d) {
    if (!d || !d->combo || d->editRow < 0) return;
    int sel = (int)SendMessageA(d->combo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && d->editRow < (int)d->rows.size()) {
        d->rows[d->editRow].preset = PresetFromComboIndex(sel);
        ListView_SetItemText(d->lv, d->editRow, COL_PRESET_VALUE,
                             const_cast<char*>(PresetLabel(d->rows[d->editRow].preset)));
    }
    ShowWindow(d->combo, SW_HIDE);
    d->editRow = -1;
    SetFocus(d->lv);
}

// Open the inline edit combo over the Behavior cell of `row`.
static void BeginInlineEdit(PresetDlgData* d, int row) {
    if (!d || !d->combo || row < 0 || row >= (int)d->rows.size()) return;
    // Position over the Behavior subitem rectangle.
    RECT rc{};
    rc.top = COL_PRESET_VALUE;   // subitem index
    rc.left = LVIR_BOUNDS;
    if (!ListView_GetSubItemRect(d->lv, row, COL_PRESET_VALUE, LVIR_BOUNDS, &rc))
        return;
    MapWindowPoints(d->lv, GetParent(d->lv), (POINT*)&rc, 2);
    d->editRow = row;
    SendMessageA(d->combo, CB_SETCURSEL, ComboIndexForPreset(d->rows[row].preset), 0);
    // Drop the list a bit taller than the cell so it's easy to use.
    SetWindowPos(d->combo, HWND_TOP, rc.left, rc.top,
                 rc.right - rc.left, 200, SWP_SHOWWINDOW);
    SetFocus(d->combo);
    SendMessageA(d->combo, CB_SHOWDROPDOWN, TRUE, 0);
}

static INT_PTR CALLBACK PresetDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<PresetDlgData*>(GetWindowLongPtrA(dlg, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        d = reinterpret_cast<PresetDlgData*>(lp);
        SetWindowLongPtrA(dlg, GWLP_USERDATA, lp);
        SetWindowTextA(dlg, "Smart Conform");

        d->lv     = GetDlgItem(dlg, IDC_PRESET_LIST);
        d->combo  = GetDlgItem(dlg, IDC_EDIT_COMBO);
        d->status = GetDlgItem(dlg, IDC_PRESET_STATUS);

        SetWindowTextA(GetDlgItem(dlg, IDC_SUBTITLE),
                       "Set how each mesh should drape against the body morph. "
                       "Click a Behavior cell to change it.");

        ListView_SetExtendedListViewStyle(d->lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNA c0{LVCF_TEXT | LVCF_WIDTH, 0, 244, const_cast<char*>("Mesh")};
        LVCOLUMNA c1{LVCF_TEXT | LVCF_WIDTH, 0, 168, const_cast<char*>("Behavior")};
        ListView_InsertColumn(d->lv, COL_PRESET_SHAPE, &c0);
        ListView_InsertColumn(d->lv, COL_PRESET_VALUE, &c1);

        // The inline edit combo is hidden until a cell is clicked.
        PopulatePresetCombo(d->combo);
        ShowWindow(d->combo, SW_HIDE);

        // Intensity: default to Medium.
        d->intensity = 1;
        SendMessageA(GetDlgItem(dlg, IDC_INT_MED), BM_SETCHECK, BST_CHECKED, 0);
        SetWindowTextA(GetDlgItem(dlg, IDC_QUALITY_DESC), QualityDesc(1));

        std::array<char, 65536> buf{};
        int count = d->api.getCurrentShapes ? d->api.getCurrentShapes(buf.data(), (int)buf.size()) : -1;
        std::istringstream ss(buf.data());
        std::string line;
        while (std::getline(ss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty()) continue;
            PresetRow row;
            row.shape = line;
            row.preset = GuessPresetForShapeUi(line);
            d->rows.push_back(std::move(row));
        }

        PopulatePresetRows(d->lv, d->rows);
        UpdatePresetStatus(d);

        if (count <= 0 || d->rows.empty()) {
            SetWindowTextA(d->status, "No meshes found. Check %TEMP%\\drape_debug.log.");
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_AUTO_DETECT), FALSE);
        } else {
            ListView_SetItemState(d->lv, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (!nm || !d) break;
        // Click on the list: open the inline editor if the Behavior cell was hit.
        if (nm->idFrom == IDC_PRESET_LIST && nm->code == NM_CLICK) {
            auto* ia = reinterpret_cast<NMITEMACTIVATE*>(lp);
            LVHITTESTINFO ht{};
            ht.pt = ia->ptAction;
            int row = ListView_SubItemHitTest(d->lv, &ht);
            if (row >= 0 && ht.iSubItem == COL_PRESET_VALUE)
                BeginInlineEdit(d, row);
            else
                CommitInlineEdit(d);
        }
        break;
    }

    case WM_COMMAND: {
        WORD id   = LOWORD(wp);
        WORD code = HIWORD(wp);

        // Inline edit combo: commit on selection, cancel/commit on close-up.
        if (id == IDC_EDIT_COMBO && d) {
            if (code == CBN_SELCHANGE || code == CBN_CLOSEUP) {
                CommitInlineEdit(d);
                return TRUE;
            }
        }

        if ((id == IDC_INT_LOW || id == IDC_INT_MED || id == IDC_INT_HIGH) && d) {
            d->intensity = (id == IDC_INT_LOW) ? 0 : (id == IDC_INT_MED) ? 1 : 2;
            SetWindowTextA(GetDlgItem(dlg, IDC_QUALITY_DESC), QualityDesc(d->intensity));
            return TRUE;
        }

        if (id == IDC_AUTO_DETECT && d) {
            CommitInlineEdit(d);
            for (int i = 0; i < (int)d->rows.size(); ++i) {
                d->rows[i].preset = GuessPresetForShapeUi(d->rows[i].shape);
                ListView_SetItemText(d->lv, i, COL_PRESET_VALUE,
                                     const_cast<char*>(PresetLabel(d->rows[i].preset)));
            }
            return TRUE;
        }

        if (id == IDOK && d) {
            if (d->api.setProcessingIntensity)
                d->api.setProcessingIntensity(d->intensity);
            if (d->api.clearShapePresets)
                d->api.clearShapePresets();
            for (const auto& row : d->rows) {
                if (d->api.setShapePreset)
                    d->api.setShapePreset(row.shape.c_str(), row.preset);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }

        if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }

    return FALSE;
}

bool ShowDrapePresetDialog(HWND parent, const DrapeHookApi& api) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    // ── Layout grid (dialog units) ────────────────────────────────────────
    // Native Windows look: standard dialog surface, group boxes for chunking,
    // edit-in-place behavior column (no redundant editor row). Clear rhythm.
    const short DW = 392, DH = 322;
    const short PAD = 10;
    const short CW  = DW - PAD * 2;

    const short ySub     = 8;                  // one-line instruction
    const short yMeshGrp = ySub + 16;
    const short meshGrpH = 170;
    const short yList    = yMeshGrp + 13;
    const short listH    = meshGrpH - 21;

    const short yQualGrp  = yMeshGrp + meshGrpH + 8;
    const short qualGrpH  = 42;
    const short yRadio    = yQualGrp + 13;
    const short yQualDesc = yQualGrp + 27;

    const short yStatus  = yQualGrp + qualGrpH + 5;
    const short yButtons = DH - 24;

    DlgBuilder b;
    // subtitle(1) meshGrp(1) list(1) editCombo(1) auto(1) qualGrp(1)
    //   + 3 radios + qualDesc(1) status(1) Conform(1) Cancel(1) = 13
    const WORD nControls = 13;
    b.dw(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_CAPTION | WS_SYSMENU | WS_POPUP);
    b.dw(0); b.w(nControls);
    b.w(0); b.w(0); b.w(DW); b.w(DH);
    b.w(0); b.w(0); b.str(L"Smart Conform");
    b.w(9); b.str(L"Segoe UI");

    // ── Instruction line ──────────────────────────────────────────────────
    b.control(SS_LEFT | SS_NOPREFIX, PAD, ySub, CW, 11, IDC_SUBTITLE, L"STATIC", L"");

    // ── Meshes group ──────────────────────────────────────────────────────
    b.control(BS_GROUPBOX, PAD, yMeshGrp, CW, meshGrpH, IDC_GRP_MESHES,
              L"BUTTON", L"Meshes");
    b.control(LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
              PAD + 7, yList, CW - 14, listH, IDC_PRESET_LIST, L"SysListView32", L"");
    // Inline edit combo — created hidden, positioned over a Behavior cell on click.
    b.control(CBS_DROPDOWNLIST | WS_VSCROLL,
              PAD + 7, yList, 160, 200, IDC_EDIT_COMBO, L"COMBOBOX", L"");
    // Auto-detect button sits just under the list, right-aligned.
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              DW - PAD - 92, yMeshGrp + meshGrpH - 16, 84, 13,
              IDC_AUTO_DETECT, L"BUTTON", L"Auto-detect all");

    // ── Quality group ─────────────────────────────────────────────────────
    b.control(BS_GROUPBOX, PAD, yQualGrp, CW, qualGrpH, IDC_GRP_QUALITY,
              L"BUTTON", L"Quality");
    b.control(BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP,
              PAD + 10, yRadio, 60, 11, IDC_INT_LOW,  L"BUTTON", L"Low");
    b.control(BS_AUTORADIOBUTTON | WS_TABSTOP,
              PAD + 76, yRadio, 74, 11, IDC_INT_MED,  L"BUTTON", L"Medium");
    b.control(BS_AUTORADIOBUTTON | WS_TABSTOP,
              PAD + 156, yRadio, 60, 11, IDC_INT_HIGH, L"BUTTON", L"High");
    b.control(SS_LEFT | SS_NOPREFIX, PAD + 10, yQualDesc, CW - 20, 10,
              IDC_QUALITY_DESC, L"STATIC", L"");

    // ── Status line ───────────────────────────────────────────────────────
    b.control(SS_LEFT | SS_NOPREFIX, PAD, yStatus, CW - 4, 10,
              IDC_PRESET_STATUS, L"STATIC", L"");

    // ── Action bar ────────────────────────────────────────────────────────
    b.control(BS_DEFPUSHBUTTON | WS_TABSTOP,
              DW - PAD - 152, yButtons, 84, 15, IDOK, L"BUTTON", L"Conform");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              DW - PAD - 64, yButtons, 64, 15, IDCANCEL, L"BUTTON", L"Cancel");

    PresetDlgData data;
    data.api = api;
    INT_PTR rc = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(b.data.data()),
        parent, PresetDlgProc,
        reinterpret_cast<LPARAM>(&data));
    return rc == IDOK;
}

void ShowDrapeDialog(HWND parent, const std::string& bsDir) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    DlgBuilder b;
    const WORD nControls = 13;

    b.dw(DS_MODALFRAME | DS_CENTER | DS_SETFONT |
         WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_POPUP);
    b.dw(0); b.w(nControls);
    b.w(0); b.w(0); b.w(420); b.w(328);
    b.w(0); b.w(0); b.str(L"Smart Conform");
    b.w(9); b.str(L"Segoe UI");

    b.control(SS_LEFT, 4, 4, 40, 10, 0, L"STATIC", L"Project:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | ES_READONLY,
              48, 2, 284, 12, IDC_OSP_EDIT, L"EDIT", L"");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              336, 2, 54, 12, IDC_OSP_BROWSE, L"BUTTON", L"Browse...");

    b.control(LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_VSCROLL,
              4, 18, 392, 168, IDC_SHAPES_LIST, L"SysListView32", L"");

    b.control(SS_LEFT, 4, 191, 42, 10, 0, L"STATIC", L"d_tight:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
              50, 189, 40, 12, IDC_D_TIGHT_EDT, L"EDIT", L"");
    b.control(SS_LEFT, 100, 191, 42, 10, 0, L"STATIC", L"d_rigid:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
              146, 189, 40, 12, IDC_D_RIGID_EDT, L"EDIT", L"");

    b.control(ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER | WS_VSCROLL,
              4, 205, 392, 90, IDC_STATUS, L"EDIT", L"");

    b.control(PBS_SMOOTH, 4, 299, 392, 8, IDC_PROGRESS, L"msctls_progress32", L"");

    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              4, 312, 120, 14, IDC_DETECT_BTN, L"BUTTON", L"Detect Zones");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              130, 312, 150, 14, IDC_CONFORM_BTN, L"BUTTON", L"Conform with Drape");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              350, 312, 60, 14, IDC_CANCEL_BTN, L"BUTTON", L"Close");

    DlgData data;
    data.bsDir = bsDir;

    DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(b.data.data()),
        parent, DrapeDlgProc,
        reinterpret_cast<LPARAM>(&data));
}
