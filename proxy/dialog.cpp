#include "dialog.h"
#include "DrapeConformer.h"
#include "DiffData.h"
#include "NifFile.hpp"

#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <set>

#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;
using namespace nifly;

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
#define WM_DRAPE_LINE  (WM_USER + 2)   // lParam = LPSTR heap string (caller frees)
#define WM_DRAPE_ZONE  (WM_USER + 3)   // wParam = item idx, lParam = LPSTR "zone|verts" (caller frees)

// ── Known reference body shape names (embedded in outfit NIFs) ────────
static bool isBodyShape(const std::string& target) {
    static const char* KNOWN[] = {
        "CBBE", "HIMBO - Body HP", "BHUNP Body", "UNP Body",
        "UUNP Body", "3BA", "TBD Body", "SAM Body", "CBBE Body",
        "3BBB Body", "Bra", "Panty",  // common CBBE accessories also in ref
    };
    for (auto* k : KNOWN)
        if (target == k) return true;
    return false;
}

// ── SliderSet info ────────────────────────────────────────────────────
struct SetInfo {
    std::string name;
    std::string dataFolder;
    std::string sourceFile;
    std::string refBodyShape;           // name of body-copy shape inside the NIF
    std::set<std::string> osdFiles;     // OSD basenames referenced by this set
};

struct DlgData {
    std::string bsDir;
    std::string ospPath;
    std::vector<SetInfo> ospSets;
    HWND lvShapes  = nullptr;
    HWND edStatus  = nullptr;
    HWND progress  = nullptr;
    HWND btnDetect = nullptr;
    HWND btnConform= nullptr;
    std::atomic<bool> running{false};
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

            // Detect reference body shape from <Shape target="CBBE" ...>
            if (cur.refBodyShape.empty()) {
                auto p = line.find("<Shape target=\"");
                if (p != std::string::npos) {
                    p += 15;
                    auto q = line.find('"', p);
                    if (q != std::string::npos) {
                        std::string target = line.substr(p, q - p);
                        if (isBodyShape(target))
                            cur.refBodyShape = target;
                    }
                }
            }

            // Collect OSD basenames from <Data ... local="true">file.osd\entry</Data>
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

// ── Window helpers ────────────────────────────────────────────────────

static std::string ProjectFromTitle(HWND osHwnd) {
    char buf[512]; GetWindowTextA(osHwnd, buf, sizeof(buf));
    std::string t(buf);
    const std::string suffix = " - Outfit Studio";
    auto p = t.rfind(suffix);
    if (p == std::string::npos) return "";
    std::string name = t.substr(0, p);
    if (!name.empty() && name.back() == '*') name.pop_back();
    return name;
}

static std::string FindOsp(const std::string& bsDir, const std::string& proj) {
    fs::path ssDir = fs::path(bsDir) / "SliderSets";
    if (!fs::is_directory(ssDir)) return "";
    for (auto& e : fs::directory_iterator(ssDir)) {
        if (e.path().extension() != ".osp") continue;
        std::ifstream f(e.path()); std::string line;
        while (std::getline(f, line))
            if (line.find("name=\"" + proj + "\"") != std::string::npos)
                return e.path().string();
    }
    return "";
}

static void PopulateSets(HWND lv, const std::vector<SetInfo>& sets) {
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)sets.size(); ++i) {
        LVITEMA item{};
        item.mask = LVIF_TEXT | LVIF_STATE;
        item.iItem = i;
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state = INDEXTOSTATEIMAGEMASK(2);  // checked
        item.pszText = const_cast<char*>(sets[i].name.c_str());
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, COL_ZONE,  const_cast<char*>("—"));
        ListView_SetItemText(lv, i, COL_VERTS, const_cast<char*>("—"));
    }
}

static void PostLog(HWND dlg, const std::string& msg) {
    PostMessageA(dlg, WM_DRAPE_LINE, 0, (LPARAM)_strdup(msg.c_str()));
}

// ── NIF helpers ───────────────────────────────────────────────────────

static std::vector<Vec3> shapeVerts(NifFile& nif, NiShape* s) {
    std::vector<Vector3> v; nif.GetVertsForShape(s, v);
    std::vector<Vec3> out; out.reserve(v.size());
    for (auto& x : v) out.push_back({x.x, x.y, x.z});
    return out;
}

static std::vector<std::array<uint16_t,3>> shapeTris(NifFile& nif, NiShape* s) {
    std::vector<Triangle> t; nif.GetTrianglesForShape(s, t);
    std::vector<std::array<uint16_t,3>> out; out.reserve(t.size());
    for (auto& x : t) out.push_back({x.p1, x.p2, x.p3});
    return out;
}

// ── Detect zones ──────────────────────────────────────────────────────

static void RunDetect(HWND dlg, DlgData* d, float dTight, float dRigid) {
    DrapeOptions opts; opts.d_tight = dTight; opts.d_rigid = dRigid;
    DrapeConformer dc(opts);

    int n = ListView_GetItemCount(d->lvShapes);
    for (int i = 0; i < n; ++i) {
        if (!ListView_GetCheckState(d->lvShapes, i)) continue;
        const SetInfo& si = d->ospSets[i];

        std::string nifPath = d->bsDir + "ShapeData\\" + si.dataFolder + "\\" + si.sourceFile;
        if (!fs::exists(nifPath)) {
            PostLog(dlg, "SKIP " + si.name + " (NIF not found)");
            continue;
        }

        NifFile nif;
        if (nif.Load(nifPath) != 0) {
            PostLog(dlg, "SKIP " + si.name + " (NIF load failed)");
            continue;
        }

        // Find the embedded reference body shape
        NiShape* refShape = nullptr;
        for (auto* s : nif.GetShapes()) {
            if (s && s->name.get() == si.refBodyShape) { refShape = s; break; }
        }
        if (!refShape) {
            PostLog(dlg, "SKIP " + si.name + " (ref body shape '" + si.refBodyShape + "' not found)");
            continue;
        }

        auto refVerts = shapeVerts(nif, refShape);
        PostLog(dlg, "Detecting: " + si.name + "  (ref: " + si.refBodyShape + ")");

        int tight = 0, loose = 0, rigid = 0;

        for (auto* s : nif.GetShapes()) {
            if (!s || s->name.get() == si.refBodyShape) continue;
            auto results = dc.detect(nifPath, nifPath);  // use same NIF, filter by shape later
            // Re-do detect per-shape using pre-loaded ref verts
            // (dc.detect loads the NIF internally; we use a lighter manual path here)
            auto outVerts = shapeVerts(nif, s);
            // Compute zones manually for efficiency
            DrapeConformer dcLocal(opts);
            (void)dcLocal;  // used only for filterDiffs via detect workaround below
            // Use detect() result on whole NIF and accumulate
            for (auto& sr : results) {
                if (sr.name != s->name.get()) continue;
                tight += sr.tightVerts; loose += sr.looseVerts; rigid += sr.rigidVerts;
            }
        }

        int total = tight + loose + rigid;
        const char* zone =
            (rigid > tight && rigid > loose) ? "Rigid" :
            (loose > tight)                  ? "Loose" : "SkinTight";

        PostLog(dlg, "  " + std::string(zone) +
                "  tight=" + std::to_string(tight) +
                "  loose=" + std::to_string(loose) +
                "  rigid=" + std::to_string(rigid));

        std::string payload = std::string(zone) + "|" + std::to_string(total);
        PostMessageA(dlg, WM_DRAPE_ZONE, (WPARAM)i, (LPARAM)_strdup(payload.c_str()));
    }
    PostMessageA(dlg, WM_DRAPE_DONE, 0, 0);
}

// ── Conform: hook Conform All + filter OSD ────────────────────────────

static void RunConform(HWND dlg, DlgData* d, float dTight, float dRigid) {
    if (g_conformAllId == 0) {
        PostLog(dlg, "ERROR: Conform All not found in OutfitStudio menu.");
        PostMessageA(dlg, WM_DRAPE_DONE, 1, 0); return;
    }

    // ── Step 1: trigger vanilla Conform All ───────────────────────────
    PostLog(dlg, "Running Conform All...");
    SendMessage(g_osHwnd, WM_COMMAND, MAKEWPARAM(g_conformAllId, 0), 0);
    PostLog(dlg, "Conform All done. Applying Drape zone filter...");

    DrapeOptions opts; opts.d_tight = dTight; opts.d_rigid = dRigid;
    DrapeConformer dc(opts);

    int n = ListView_GetItemCount(d->lvShapes);
    int totalSets = 0;

    for (int i = 0; i < n; ++i) {
        if (!ListView_GetCheckState(d->lvShapes, i)) continue;
        const SetInfo& si = d->ospSets[i];

        std::string nifPath = d->bsDir + "ShapeData\\" + si.dataFolder + "\\" + si.sourceFile;
        if (!fs::exists(nifPath)) {
            PostLog(dlg, "SKIP " + si.name + " (NIF not found)"); continue;
        }

        // ── Step 2: load NIF, find embedded ref body shape ────────────
        NifFile nif;
        if (nif.Load(nifPath) != 0) {
            PostLog(dlg, "SKIP " + si.name + " (NIF load failed)"); continue;
        }

        NiShape* refShape = nullptr;
        for (auto* s : nif.GetShapes())
            if (s && s->name.get() == si.refBodyShape) { refShape = s; break; }

        if (!refShape) {
            PostLog(dlg, "WARN " + si.name + ": ref shape '" + si.refBodyShape +
                    "' not found — filtering with largest shape as ref");
            int maxV = 0;
            for (auto* s : nif.GetShapes()) {
                if (!s) continue;
                std::vector<Vector3> v; nif.GetVertsForShape(s, v);
                if ((int)v.size() > maxV) { maxV = (int)v.size(); refShape = s; }
            }
        }
        if (!refShape) {
            PostLog(dlg, "SKIP " + si.name + " (no shapes in NIF)"); continue;
        }

        auto refVerts = shapeVerts(nif, refShape);
        std::string refName = refShape->name.get();

        PostLog(dlg, "Filtering: " + si.name + "  (ref: " + refName + ")");

        // ── Step 3: for each OSD, filter non-body shapes ──────────────
        for (const std::string& osdFile : si.osdFiles) {
            std::string osdPath = d->bsDir + "ShapeData\\" + si.dataFolder + "\\" + osdFile;
            if (!fs::exists(osdPath)) continue;

            OSDataFile osd;
            if (!osd.Read(osdPath)) {
                PostLog(dlg, "  SKIP " + osdFile + " (read failed)"); continue;
            }

            bool modified = false;
            for (auto* shape : nif.GetShapes()) {
                if (!shape) continue;
                std::string shapeName = shape->name.get();
                if (shapeName == refName) continue;  // skip the body copy itself

                auto outVerts = shapeVerts(nif, shape);
                auto outTris  = shapeTris(nif, shape);

                for (auto& [key, tdiffs] : osd.GetDataDiffs()) {
                    if (!tdiffs) continue;
                    if (key.substr(0, shapeName.size()) != shapeName) continue;

                    VertexDiffs vd;
                    for (auto& [idx, v] : *tdiffs) vd[idx] = {v.x, v.y, v.z};

                    auto filtered = dc.filterDiffs(outVerts, refVerts, outTris, vd);

                    TargetDataDiffs tdd;
                    for (auto& [idx, v] : filtered) tdd[idx] = {v.x, v.y, v.z};
                    osd.SetDataDiff(key, tdd);
                    modified = true;
                }
            }

            if (modified) {
                if (osd.Write(osdPath))
                    PostLog(dlg, "  Written: " + osdFile);
                else
                    PostLog(dlg, "  ERROR writing: " + osdFile);
            }
        }
        ++totalSets;
    }

    PostLog(dlg, totalSets > 0
        ? "Done — " + std::to_string(totalSets) + " set(s) filtered.\n"
          "Reload the project in OutfitStudio to see the result."
        : "Nothing was filtered.");

    PostMessageA(dlg, WM_DRAPE_DONE, 0, 0);
}

// ── Dialog procedure ──────────────────────────────────────────────────

static INT_PTR CALLBACK DrapeDlgProc(HWND dlg, UINT msg,
                                      WPARAM wp, LPARAM lp) {
    DlgData* d = reinterpret_cast<DlgData*>(GetWindowLongPtrA(dlg, GWLP_USERDATA));

    switch (msg) {

    case WM_INITDIALOG: {
        d = reinterpret_cast<DlgData*>(lp);
        SetWindowLongPtrA(dlg, GWLP_USERDATA, lp);
        SetWindowTextA(dlg, "Drape Conform");

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

        // Auto-load project from OutfitStudio window title
        HWND osWnd = GetParent(dlg);
        std::string proj = ProjectFromTitle(osWnd);
        if (!proj.empty()) {
            SetDlgItemTextA(dlg, IDC_OSP_EDIT, proj.c_str());
            std::string osp = FindOsp(d->bsDir, proj);
            if (!osp.empty()) {
                d->ospPath = osp;
                d->ospSets = ParseOspSets(osp);
                PopulateSets(lv, d->ospSets);
            }
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
                MessageBoxA(dlg, "Load a project first.", "Drape", MB_OK); break;
            }
            char tight[16], rigid[16];
            GetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, tight, sizeof(tight));
            GetDlgItemTextA(dlg, IDC_D_RIGID_EDT, rigid, sizeof(rigid));
            d->running = true;
            EnableWindow(d->btnDetect, FALSE); EnableWindow(d->btnConform, FALSE);
            SetWindowTextA(d->edStatus, "");
            SendMessageA(d->progress, PBM_SETPOS, 0, 0);
            float dT = (float)atof(tight), dR = (float)atof(rigid);
            std::thread([=]{ RunDetect(dlg, d, dT, dR); }).detach();
        }

        else if (id == IDC_CONFORM_BTN && !d->running) {
            if (d->ospSets.empty()) {
                MessageBoxA(dlg, "Load a project first.", "Drape", MB_OK); break;
            }
            char tight[16], rigid[16];
            GetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, tight, sizeof(tight));
            GetDlgItemTextA(dlg, IDC_D_RIGID_EDT, rigid, sizeof(rigid));
            d->running = true;
            EnableWindow(d->btnDetect, FALSE); EnableWindow(d->btnConform, FALSE);
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
        if (line) {
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
        if (payload) {
            std::istringstream ss(payload);
            std::string zone, verts;
            std::getline(ss, zone,  '|');
            std::getline(ss, verts, '|');
            ListView_SetItemText(d->lvShapes, item, COL_ZONE,
                                 const_cast<char*>(zone.c_str()));
            ListView_SetItemText(d->lvShapes, item, COL_VERTS,
                                 const_cast<char*>(verts.c_str()));
            free(payload);
        }
        break;
    }

    case WM_DRAPE_DONE: {
        d->running = false;
        EnableWindow(d->btnDetect, TRUE); EnableWindow(d->btnConform, TRUE);
        SendMessageA(d->progress, PBM_SETPOS, (DWORD)wp == 0 ? 100 : 0, 0);
        break;
    }

    case WM_SIZE: {
        if (!d) break;
        RECT cr; GetClientRect(dlg, &cr);
        int w = cr.right - 16, top = 32;
        SetWindowPos(GetDlgItem(dlg, IDC_SHAPES_LIST), nullptr,
                     8, top, w, cr.bottom - top - 175, SWP_NOZORDER);
        int statusTop = cr.bottom - 159;
        SetWindowPos(d->edStatus, nullptr,
                     8, statusTop, w, 110, SWP_NOZORDER);
        int btnY = cr.bottom - 40;
        SetWindowPos(d->btnDetect,  nullptr, 8,      btnY, 120, 28, SWP_NOZORDER);
        SetWindowPos(d->btnConform, nullptr, 136,    btnY, 150, 28, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(dlg, IDC_CANCEL_BTN),
                                  nullptr, w - 84,  btnY,  84, 28, SWP_NOZORDER);
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

void ShowDrapeDialog(HWND parent, const std::string& bsDir) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    // Layout (420 × 328 DLU):
    //  y=2:  [Project:] [edit] [Browse...]
    //  y=16: [ListView]
    //  y=191:[d_tight:] [edit] [d_rigid:] [edit]
    //  y=205:[Status edit]
    //  y=299:[Progress]
    //  y=306:[Detect Zones] [Conform with Drape] [Close]

    DlgBuilder b;
    const WORD nControls = 11;

    b.dw(DS_MODALFRAME | DS_CENTER | DS_SETFONT |
         WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_POPUP);
    b.dw(0); b.w(nControls);
    b.w(0); b.w(0); b.w(420); b.w(328);
    b.w(0); b.w(0); b.str(L"Drape Conform");
    b.w(9); b.str(L"Segoe UI");

    // Row 1 — Project
    b.control(SS_LEFT, 4, 4, 40, 10, 0, L"STATIC", L"Project:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | ES_READONLY,
              48, 2, 284, 12, IDC_OSP_EDIT, L"EDIT", L"");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              336, 2, 54, 12, IDC_OSP_BROWSE, L"BUTTON", L"Browse...");

    // ListView
    b.control(LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_VSCROLL,
              4, 18, 392, 168, IDC_SHAPES_LIST, L"SysListView32", L"");

    // Thresholds
    b.control(SS_LEFT, 4, 191, 42, 10, 0, L"STATIC", L"d_tight:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
              50, 189, 40, 12, IDC_D_TIGHT_EDT, L"EDIT", L"");
    b.control(SS_LEFT, 100, 191, 42, 10, 0, L"STATIC", L"d_rigid:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
              146, 189, 40, 12, IDC_D_RIGID_EDT, L"EDIT", L"");

    // Status
    b.control(ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER | WS_VSCROLL,
              4, 205, 392, 90, IDC_STATUS, L"EDIT", L"");

    // Progress
    b.control(PBS_SMOOTH, 4, 299, 392, 8, IDC_PROGRESS, L"msctls_progress32", L"");

    // Buttons
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
