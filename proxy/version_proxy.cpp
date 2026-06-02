#define VER_H       // prevent winver.h from declaring VerFindFile* / VerInstallFile*
#include <windows.h>

// Load the real system version.dll once.
// LOAD_LIBRARY_SEARCH_SYSTEM32 ensures we get the system copy, not ourselves.
static HMODULE LoadReal() {
    static HMODULE h = LoadLibraryExW(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return h;
}
static FARPROC Sym(const char* name) {
    return GetProcAddress(LoadReal(), name);
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
