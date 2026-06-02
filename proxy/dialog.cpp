#include "dialog.h"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>

#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;

// ── Control IDs ──────────────────────────────────────────────────────
enum : UINT {
    IDC_PROJECT_LBL = 101,
    IDC_OSP_EDIT    = 102,
    IDC_OSP_BROWSE  = 103,
    IDC_SHAPES_LIST = 104,
    IDC_DETECT_BTN  = 105,
    IDC_CONFORM_BTN = 106,
    IDC_CANCEL_BTN  = 107,
    IDC_PROGRESS    = 108,
    IDC_STATUS      = 109,
    IDC_D_TIGHT_EDT = 110,
    IDC_D_RIGID_EDT = 111,
    // ListView columns
    COL_CHECK = 0, COL_SHAPE = 0, COL_ZONE = 1, COL_DIST = 2,
};
// Custom message posted from worker thread
#define WM_DRAPE_DONE  (WM_USER + 1)
#define WM_DRAPE_LINE  (WM_USER + 2)   // lParam = new LPSTR (caller frees)

// ── Data for the dialog proc ─────────────────────────────────────────
struct DlgData {
    std::string bsDir;
    std::string ospPath;
    HWND        lvShapes  = nullptr;
    HWND        edStatus  = nullptr;
    HWND        progress  = nullptr;
    HWND        btnDetect = nullptr;
    HWND        btnConform= nullptr;
    std::atomic<bool> running{false};
};

// ── Helpers ──────────────────────────────────────────────────────────

// Read OutfitStudio window title → extract project name
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

// Find the .osp file for a project name in bsDir/SliderSets/
static std::string FindOsp(const std::string& bsDir, const std::string& projName) {
    fs::path ssDir = fs::path(bsDir) / "SliderSets";
    if (!fs::is_directory(ssDir)) return "";
    for (auto& e : fs::directory_iterator(ssDir)) {
        if (e.path().extension() != ".osp") continue;
        // Check if any SliderSet in the file matches projName
        std::ifstream f(e.path());
        std::string line;
        while (std::getline(f, line))
            if (line.find("name=\"" + projName + "\"") != std::string::npos)
                return e.path().string();
    }
    return "";
}

// Parse SliderSet names from an .osp file
static std::vector<std::string> ParseShapes(const std::string& ospPath) {
    std::vector<std::string> out;
    std::ifstream f(ospPath);
    std::string line;
    while (std::getline(f, line)) {
        auto p = line.find("<SliderSet name=\"");
        if (p == std::string::npos) continue;
        p += 17;
        auto q = line.find('"', p);
        if (q != std::string::npos)
            out.push_back(line.substr(p, q - p));
    }
    return out;
}

// Run drape-cli and return stdout; post WM_DRAPE_LINE lines while running
static std::string RunCli(HWND dlg, const std::string& exe,
                           const std::vector<std::string>& args) {
    // Build command line
    std::string cmd = "\"" + exe + "\"";
    for (auto& a : args) cmd += " " + a;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rPipe, wPipe;
    CreatePipe(&rPipe, &wPipe, &sa, 0);
    SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{sizeof(si)};
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = wPipe;
    si.hStdError   = wPipe;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wPipe);

    std::string full;
    char buf[2048]; DWORD got;
    std::string lineBuf;
    while (ReadFile(rPipe, buf, sizeof(buf) - 1, &got, nullptr) && got) {
        buf[got] = 0;
        full += buf;
        lineBuf += buf;
        // Post each complete line to the dialog
        size_t nl;
        while ((nl = lineBuf.find('\n')) != std::string::npos) {
            std::string ln = lineBuf.substr(0, nl);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            char* copy = _strdup(ln.c_str());
            PostMessageA(dlg, WM_DRAPE_LINE, 0, (LPARAM)copy);
            lineBuf = lineBuf.substr(nl + 1);
        }
    }
    DWORD exit_code = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rPipe);
    PostMessageA(dlg, WM_DRAPE_DONE, exit_code, 0);
    return full;
}

// Populate the shapes ListView
static void PopulateShapes(HWND lv, const std::vector<std::string>& shapes) {
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < (int)shapes.size(); ++i) {
        LVITEMA item{};
        item.mask    = LVIF_TEXT | LVIF_STATE;
        item.iItem   = i;
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.state   = INDEXTOSTATEIMAGEMASK(2); // checked
        item.pszText = const_cast<char*>(shapes[i].c_str());
        ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, i, COL_ZONE, const_cast<char*>("—"));
        ListView_SetItemText(lv, i, COL_DIST, const_cast<char*>("—"));
    }
}

// ── Dialog procedure ─────────────────────────────────────────────────

static INT_PTR CALLBACK DrapeDlgProc(HWND dlg, UINT msg,
                                      WPARAM wp, LPARAM lp) {
    DlgData* d = reinterpret_cast<DlgData*>(GetWindowLongPtrA(dlg, GWLP_USERDATA));

    switch (msg) {

    case WM_INITDIALOG: {
        d = reinterpret_cast<DlgData*>(lp);
        SetWindowLongPtrA(dlg, GWLP_USERDATA, lp);

        SetWindowTextA(dlg, "Drape Conform");

        // ListView extended styles (checkboxes + gridlines)
        HWND lv = GetDlgItem(dlg, IDC_SHAPES_LIST);
        d->lvShapes  = lv;
        d->edStatus  = GetDlgItem(dlg, IDC_STATUS);
        d->progress  = GetDlgItem(dlg, IDC_PROGRESS);
        d->btnDetect = GetDlgItem(dlg, IDC_DETECT_BTN);
        d->btnConform= GetDlgItem(dlg, IDC_CONFORM_BTN);

        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Columns
        auto addCol = [&](int i, const char* text, int w) {
            LVCOLUMNA c{LVCF_TEXT | LVCF_WIDTH, 0, w,
                        const_cast<char*>(text)};
            ListView_InsertColumn(lv, i, &c);
        };
        RECT r; GetClientRect(lv, &r);
        int total = r.right - r.left;
        addCol(COL_SHAPE, "Shape",     total - 160);
        addCol(COL_ZONE,  "Zone",      80);
        addCol(COL_DIST,  "Min Dist",  80);

        // Default threshold values
        SetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, "2.0");
        SetDlgItemTextA(dlg, IDC_D_RIGID_EDT, "20.0");

        // Try to populate from current OutfitStudio project
        HWND osWnd = GetParent(dlg);
        std::string proj = ProjectFromTitle(osWnd);
        if (!proj.empty()) {
            SetDlgItemTextA(dlg, IDC_OSP_EDIT, proj.c_str());
            std::string osp = FindOsp(d->bsDir, proj);
            if (!osp.empty()) {
                d->ospPath = osp;
                PopulateShapes(lv, ParseShapes(osp));
            }
        }

        SendMessageA(d->progress, PBM_SETRANGE32, 0, 100);
        return TRUE;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wp);

        if (id == IDC_OSP_BROWSE) {
            // Simple open dialog for .osp file
            char path[MAX_PATH] = "";
            OPENFILENAMEA ofn{sizeof(ofn)};
            ofn.hwndOwner   = dlg;
            ofn.lpstrFilter = "Outfit Studio Project\0*.osp\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameA(&ofn)) {
                d->ospPath = path;
                // show just filename in edit
                SetDlgItemTextA(dlg, IDC_OSP_EDIT,
                    fs::path(path).stem().string().c_str());
                PopulateShapes(d->lvShapes, ParseShapes(path));
            }
        }

        else if (id == IDC_DETECT_BTN && !d->running) {
            if (d->ospPath.empty()) {
                MessageBoxA(dlg, "Select a project first.", "Drape", MB_OK);
                break;
            }
            d->running = true;
            EnableWindow(d->btnDetect,  FALSE);
            EnableWindow(d->btnConform, FALSE);
            SetWindowTextA(d->edStatus, "Detecting cloth zones...\r\n");
            SendMessageA(d->progress, PBM_SETPOS, 0, 0);

            char tight[16], rigid[16];
            GetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, tight, sizeof(tight));
            GetDlgItemTextA(dlg, IDC_D_RIGID_EDT, rigid, sizeof(rigid));
            std::string cli = d->bsDir + "drape-cli.exe";
            std::vector<std::string> args = {
                "--detect",
                "--input-osp", "\"" + d->ospPath + "\"",
                "--d-tight", tight,
                "--d-rigid", rigid
            };
            std::thread([=]{ RunCli(dlg, cli, args); }).detach();
        }

        else if (id == IDC_CONFORM_BTN && !d->running) {
            if (d->ospPath.empty()) {
                MessageBoxA(dlg, "Select a project first.", "Drape", MB_OK);
                break;
            }
            // Collect checked shapes
            int n = ListView_GetItemCount(d->lvShapes);
            std::vector<std::string> shapes;
            for (int i = 0; i < n; ++i) {
                if (ListView_GetCheckState(d->lvShapes, i)) {
                    char buf[256];
                    ListView_GetItemTextA(d->lvShapes, i, 0, buf, sizeof(buf));
                    shapes.push_back(buf);
                }
            }
            if (shapes.empty()) {
                MessageBoxA(dlg, "Check at least one shape.", "Drape", MB_OK);
                break;
            }
            d->running = true;
            EnableWindow(d->btnDetect,  FALSE);
            EnableWindow(d->btnConform, FALSE);
            SetWindowTextA(d->edStatus, "Running Drape conform...\r\n");
            SendMessageA(d->progress, PBM_SETPOS, 0, 0);

            char tight[16], rigid[16];
            GetDlgItemTextA(dlg, IDC_D_TIGHT_EDT, tight, sizeof(tight));
            GetDlgItemTextA(dlg, IDC_D_RIGID_EDT, rigid, sizeof(rigid));

            // Write temp shapes file
            std::string tmpShapes = d->bsDir + "drape_shapes_tmp.txt";
            { std::ofstream f(tmpShapes);
              for (auto& s : shapes) f << s << "\n"; }

            std::string cli = d->bsDir + "drape-cli.exe";
            std::vector<std::string> args = {
                "--conform",
                "--input-osp",    "\"" + d->ospPath + "\"",
                "--shapes-file",  "\"" + tmpShapes + "\"",
                "--d-tight", tight,
                "--d-rigid", rigid
            };
            std::thread([=]{ RunCli(dlg, cli, args); }).detach();
        }

        else if (id == IDC_CANCEL_BTN || id == IDCANCEL) {
            EndDialog(dlg, 0);
        }
        break;
    }

    case WM_DRAPE_LINE: {
        // Append line to status edit
        char* line = reinterpret_cast<char*>(lp);
        if (line) {
            int len = GetWindowTextLengthA(d->edStatus);
            SendMessageA(d->edStatus, EM_SETSEL, len, len);
            SendMessageA(d->edStatus, EM_REPLACESEL, FALSE, (LPARAM)(std::string(line) + "\r\n").c_str());
            free(line);
        }
        break;
    }

    case WM_DRAPE_DONE: {
        d->running = false;
        EnableWindow(d->btnDetect,  TRUE);
        EnableWindow(d->btnConform, TRUE);
        DWORD exitCode = (DWORD)wp;
        SendMessageA(d->progress, PBM_SETPOS, exitCode == 0 ? 100 : 0, 0);
        if (exitCode == 0)
            SendMessageA(d->edStatus, EM_REPLACESEL, FALSE,
                         (LPARAM)"Done.\r\n");
        else
            SendMessageA(d->edStatus, EM_REPLACESEL, FALSE,
                         (LPARAM)"Error — check log above.\r\n");
        break;
    }

    case WM_SIZE: {
        // Basic resize: stretch listview and status
        RECT cr; GetClientRect(dlg, &cr);
        int w = cr.right - 16, top = 52;
        SetWindowPos(GetDlgItem(dlg, IDC_SHAPES_LIST), nullptr,
                     8, top, w, cr.bottom - top - 190, SWP_NOZORDER);
        int statusTop = cr.bottom - 174;
        SetWindowPos(d->edStatus, nullptr,
                     8, statusTop, w, 120, SWP_NOZORDER);
        // Reflow bottom controls
        int btnY = cr.bottom - 46;
        SetWindowPos(d->btnDetect,  nullptr, 8,        btnY, 130, 30, SWP_NOZORDER);
        SetWindowPos(d->btnConform, nullptr, 148,      btnY, 150, 30, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(dlg, IDC_CANCEL_BTN), nullptr, w - 90, btnY, 90, 30, SWP_NOZORDER);
        break;
    }

    } // switch
    return FALSE;
}

// ── Build dialog template in memory (no .rc file needed) ─────────────

struct DlgBuilder {
    std::vector<WORD> data;
    void w(WORD v)  { data.push_back(v); }
    void dw(DWORD v){ w(LOWORD(v)); w(HIWORD(v)); }
    void str(const wchar_t* s) {
        for (; *s; ++s) w((WORD)*s);
        w(0);
    }
    void align() { if (data.size() & 1) w(0); }

    void header(DWORD style, short x, short y, short cx, short cy,
                const wchar_t* title) {
        dw(style); dw(0);
        w(0);           // no menu
        w(0x0000);      // default class
        str(title);
        w(cx); w(cy);   // font size (ignored without DS_SETFONT)
    }

    void control(DWORD style, short x, short y, short cx, short cy,
                 WORD id, const wchar_t* cls, const wchar_t* text) {
        align();
        dw(style | WS_CHILD | WS_VISIBLE);
        dw(0);
        w(x); w(y); w(cx); w(cy);
        w(id);
        str(cls); str(text);
        w(0); // extra data size
    }
};

// ── Public entry point ────────────────────────────────────────────────

void ShowDrapeDialog(HWND parent, const std::string& bsDir) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    // Build DLGTEMPLATE in memory
    // Layout (560x480):
    //  [Label "Project:"] [Edit osp name] [Browse...]
    //  [ListView shapes]
    //  [Label "d_tight:"] [Edit] [Label "d_rigid:"] [Edit]
    //  [Edit status (multi-line, read-only)]
    //  [Detect Zones] [Conform with Drape] [Close]

    DlgBuilder b;
    WORD nControls = 12;

    // DLGTEMPLATE
    b.dw(DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_POPUP);
    b.dw(0);            // extended style
    b.w(nControls);
    b.w(0); b.w(0);     // x, y  (DS_CENTER handles it)
    b.w(420); b.w(340); // cx, cy in DLU
    b.w(0);             // no menu
    b.w(0);             // default class
    b.str(L"Drape Conform");
    b.w(9); b.str(L"Segoe UI"); // font

    // Controls (all coords in DLUs)
    b.control(SS_LEFT, 4, 4, 42, 10, 0, L"STATIC", L"Project:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | ES_READONLY,
              48, 2, 294, 12, IDC_OSP_EDIT, L"EDIT", L"");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              346, 2, 50, 12, IDC_OSP_BROWSE, L"BUTTON", L"Browse...");

    b.control(LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_VSCROLL,
              4, 18, 392, 170, IDC_SHAPES_LIST, L"SysListView32", L"");

    b.control(SS_LEFT, 4, 193, 42, 10, 0, L"STATIC", L"d_tight:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
              50, 191, 40, 12, IDC_D_TIGHT_EDT, L"EDIT", L"");
    b.control(SS_LEFT, 100, 193, 42, 10, 0, L"STATIC", L"d_rigid:");
    b.control(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
              146, 191, 40, 12, IDC_D_RIGID_EDT, L"EDIT", L"");

    b.control(ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
              WS_BORDER | WS_VSCROLL,
              4, 208, 392, 88, IDC_STATUS, L"EDIT", L"");

    b.control(PBS_SMOOTH, 4, 300, 392, 10, IDC_PROGRESS, L"msctls_progress32", L"");

    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              4, 316, 100, 18, IDC_DETECT_BTN, L"BUTTON", L"Detect Zones");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              110, 316, 120, 18, IDC_CONFORM_BTN, L"BUTTON", L"Conform with Drape");
    b.control(BS_PUSHBUTTON | WS_TABSTOP,
              340, 316, 56, 18, IDC_CANCEL_BTN, L"BUTTON", L"Close");

    DlgData data;
    data.bsDir = bsDir;

    DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(b.data.data()),
        parent,
        DrapeDlgProc,
        reinterpret_cast<LPARAM>(&data));
}
