#define VER_H       // prevent winver.h from declaring VerFindFile* / VerInstallFile*
#include <windows.h>
#include <cwchar>

// Load the real system version.dll once.
//
// IMPORTANT: passing only L"version.dll" is not safe under MO2/usvfs.  Its
// LoadLibrary hook reroutes the bare name to the highest-priority mod copy
// before the Windows LOAD_LIBRARY_SEARCH_SYSTEM32 policy gets a chance to
// select System32.  Since this DLL is itself named version.dll, that makes the
// forwarded exports resolve back to the proxy and tail-loop on the caller.
// Build and validate the absolute System32 path instead.
static HMODULE LoadReal() {
    static HMODULE h = []() -> HMODULE {
        wchar_t systemDir[MAX_PATH] = {};
        const UINT dirLen = GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir));
        if (dirLen == 0 || dirLen >= ARRAYSIZE(systemDir)) return nullptr;

        wchar_t systemDll[MAX_PATH] = {};
        if (swprintf_s(systemDll, L"%ls\\version.dll", systemDir) < 0) return nullptr;

        HMODULE real = LoadLibraryExW(
            systemDll,
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!real) return nullptr;

        // Do not trust the loader flags alone: verify the module that usvfs
        // actually returned.  A mismatch must never be used for forwarding.
        wchar_t loadedPath[MAX_PATH] = {};
        const DWORD loadedLen = GetModuleFileNameW(real, loadedPath, ARRAYSIZE(loadedPath));
        if (loadedLen == 0 || loadedLen >= ARRAYSIZE(loadedPath) ||
            _wcsicmp(loadedPath, systemDll) != 0) {
            FreeLibrary(real);
            return nullptr;
        }

        return real;
    }();
    return h;
}
static FARPROC Sym(const char* name) {
    HMODULE real = LoadReal();
    FARPROC proc = real ? GetProcAddress(real, name) : nullptr;
    if (!proc) {
        // A clean fail-fast is preferable to jumping through a null pointer or
        // ever falling back to another proxy copy.
        RaiseFailFastException(nullptr, nullptr, 0);
    }
    return proc;
}

// ── Exports ──────────────────────────────────────────────────────────

extern "C" {

BOOL WINAPI GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD l, LPVOID d) {
    return ((decltype(&GetFileVersionInfoA))Sym(__func__))(f, h, l, d);
}
BOOL WINAPI GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD l, LPVOID d) {
    return ((decltype(&GetFileVersionInfoW))Sym(__func__))(f, h, l, d);
}
BOOL WINAPI GetFileVersionInfoExA(DWORD fl, LPCSTR f, DWORD h, DWORD l, LPVOID d) {
    return ((decltype(&GetFileVersionInfoExA))Sym(__func__))(fl, f, h, l, d);
}
BOOL WINAPI GetFileVersionInfoExW(DWORD fl, LPCWSTR f, DWORD h, DWORD l, LPVOID d) {
    return ((decltype(&GetFileVersionInfoExW))Sym(__func__))(fl, f, h, l, d);
}
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR f, LPDWORD h) {
    return ((decltype(&GetFileVersionInfoSizeA))Sym(__func__))(f, h);
}
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD h) {
    return ((decltype(&GetFileVersionInfoSizeW))Sym(__func__))(f, h);
}
DWORD WINAPI GetFileVersionInfoSizeExA(DWORD fl, LPCSTR f, LPDWORD h) {
    return ((decltype(&GetFileVersionInfoSizeExA))Sym(__func__))(fl, f, h);
}
DWORD WINAPI GetFileVersionInfoSizeExW(DWORD fl, LPCWSTR f, LPDWORD h) {
    return ((decltype(&GetFileVersionInfoSizeExW))Sym(__func__))(fl, f, h);
}
DWORD WINAPI GetFileVersionInfoByHandle(DWORD fl, HANDLE hf, LPVOID buf, DWORD len) {
    return ((decltype(&GetFileVersionInfoByHandle))Sym(__func__))(fl, hf, buf, len);
}
DWORD WINAPI VerFindFileA(DWORD fl, LPSTR fn, LPSTR windir, LPSTR appdir, LPSTR cur, PUINT clen, LPSTR dest, PUINT dlen) {
    return ((decltype(&VerFindFileA))Sym(__func__))(fl, fn, windir, appdir, cur, clen, dest, dlen);
}
DWORD WINAPI VerFindFileW(DWORD fl, LPWSTR fn, LPWSTR windir, LPWSTR appdir, LPWSTR cur, PUINT clen, LPWSTR dest, PUINT dlen) {
    return ((decltype(&VerFindFileW))Sym(__func__))(fl, fn, windir, appdir, cur, clen, dest, dlen);
}
DWORD WINAPI VerInstallFileA(DWORD fl, LPSTR srcf, LPSTR destf, LPSTR srcdir, LPSTR destdir, LPSTR curdir, LPSTR tmpf, PUINT tlen) {
    return ((decltype(&VerInstallFileA))Sym(__func__))(fl, srcf, destf, srcdir, destdir, curdir, tmpf, tlen);
}
DWORD WINAPI VerInstallFileW(DWORD fl, LPWSTR srcf, LPWSTR destf, LPWSTR srcdir, LPWSTR destdir, LPWSTR curdir, LPWSTR tmpf, PUINT tlen) {
    return ((decltype(&VerInstallFileW))Sym(__func__))(fl, srcf, destf, srcdir, destdir, curdir, tmpf, tlen);
}
DWORD WINAPI VerLanguageNameA(DWORD wlang, LPSTR szlang, DWORD nsize) {
    return ((decltype(&VerLanguageNameA))Sym(__func__))(wlang, szlang, nsize);
}
DWORD WINAPI VerLanguageNameW(DWORD wlang, LPWSTR szlang, DWORD nsize) {
    return ((decltype(&VerLanguageNameW))Sym(__func__))(wlang, szlang, nsize);
}
BOOL WINAPI VerQueryValueA(LPCVOID blk, LPCSTR sub, LPVOID* buf, PUINT len) {
    return ((decltype(&VerQueryValueA))Sym(__func__))(blk, sub, buf, len);
}
BOOL WINAPI VerQueryValueW(LPCVOID blk, LPCWSTR sub, LPVOID* buf, PUINT len) {
    return ((decltype(&VerQueryValueW))Sym(__func__))(blk, sub, buf, len);
}

} // extern "C"
