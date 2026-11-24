#pragma once
// Minimal function-entry redirect: overwrites the first 12 bytes of a game function with
// `mov rax, target; jmp rax`. There is no trampoline -- only for functions we replace
// outright (never call the original while patched). Remove() restores the original bytes.

namespace nl::patch {

struct Redirect {
    void* at = nullptr;
    unsigned char saved[12] = {};
    bool active = false;

    bool install(void* function, void* replacement);
    void remove();
};

} // namespace nl::patch
