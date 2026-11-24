#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace nl {

static std::wstring g_dir;
static FILE* g_log = nullptr;
static std::mutex g_mu;
static ULONGLONG g_t0 = 0;

const std::wstring& mod_dir() { return g_dir; }
void set_mod_dir(const std::wstring& dir) { g_dir = dir; }

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void log_open() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_log) return;
    g_t0 = GetTickCount64();
    std::wstring dir = g_dir + L"\\log";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"\\ninelink_%04d%02d%02d-%02d%02d%02d.log", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    // Shared for reading so the launcher / an editor can tail it while the game runs.
    g_log = _wfsopen((dir + name).c_str(), L"wb", _SH_DENYWR);
}

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_log) return;
    ULONGLONG ms = GetTickCount64() - g_t0;
    fprintf(g_log, "[%6llu.%03llu] %s\n", ms / 1000, ms % 1000, buf);
    fflush(g_log);
}

} // namespace nl
