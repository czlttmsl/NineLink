// ninelink.dll -- injected into 9Kings.exe by ninelink_loader.exe.
//
// Phase 1: find the IL2CPP runtime, attach to it and write a dump of the
// game's classes so the multiplayer hooks can be planned against real names.
#include "dumper.h"
#include "il2cpp.h"
#include "log.h"
#include "perks.h"
#include "run.h"
#include "snapshot.h"
#include "sync.h"
#include "ui.h"

#include <windows.h>
#include <string>

static HMODULE g_self = nullptr;

static std::wstring dir_of_module(HMODULE m) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static DWORD pe_timestamp(HMODULE m) {
    auto dos = (IMAGE_DOS_HEADER*)m;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

static DWORD WINAPI init_thread(LPVOID) {
    nl::set_mod_dir(dir_of_module(g_self));
    nl::log_open();
    nl::logf("NineLink phase 1 -- pid %lu", GetCurrentProcessId());

    // The loader injects after the game window exists, so GameAssembly.dll is
    // normally already there; wait anyway in case it injected early.
    HMODULE ga = nullptr;
    for (int i = 0; i < 600 && !(ga = GetModuleHandleW(L"GameAssembly.dll")); ++i) Sleep(100);
    if (!ga) {
        nl::logf("GameAssembly.dll never loaded -- giving up");
        return 1;
    }
    DWORD stamp = pe_timestamp(ga);
    nl::logf("GameAssembly.dll at %p, PE timestamp 0x%08lX", (void*)ga, stamp);

    int missing = il2cpp::resolve(ga);
    nl::logf("il2cpp: exports resolved, %d missing", missing);
    if (missing) return 1;

    Il2CppDomain* domain = nullptr;
    for (int i = 0; i < 600 && !(domain = il2cpp::domain_get()); ++i) Sleep(100);
    if (!domain) {
        nl::logf("il2cpp: no domain -- runtime not initialised");
        return 1;
    }
    Il2CppThread* th = il2cpp::thread_attach(domain);
    nl::logf("il2cpp: attached to domain %p (thread %p)", (void*)domain, (void*)th);

    wchar_t name[64];
    swprintf_s(name, L"\\log\\dump_%08lX.cs", stamp);
    std::wstring path = nl::mod_dir() + name;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        nl::logf("dump: %s already exists for this game build -- skipped", nl::narrow(path).c_str());
    } else {
        ULONGLONG t = GetTickCount64();
        nl::dump_types(path);
        nl::logf("dump: took %llu ms", GetTickCount64() - t);
    }

    nl::run::init();
    nl::perks::init();
    nl::snapshot::init();
    nl::sync::install();
    nl::ui::install();

    il2cpp::thread_detach(th);
    nl::logf("init done");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        HANDLE h = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
