#include "patch.h"

#include <windows.h>
#include <cstring>

namespace nl::patch {

bool Redirect::install(void* function, void* replacement) {
    if (active || !function || !replacement) return false;
    unsigned char code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax, imm64; jmp rax
    memcpy(code + 2, &replacement, 8);
    DWORD old;
    if (!VirtualProtect(function, sizeof code, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(saved, function, sizeof saved);
    memcpy(function, code, sizeof code);
    VirtualProtect(function, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), function, sizeof code);
    at = function;
    active = true;
    return true;
}

void Redirect::remove() {
    if (!active) return;
    DWORD old;
    if (VirtualProtect(at, sizeof saved, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(at, saved, sizeof saved);
        VirtualProtect(at, sizeof saved, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof saved);
    }
    active = false;
}

} // namespace nl::patch
