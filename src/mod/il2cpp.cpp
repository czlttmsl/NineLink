#include "il2cpp.h"
#include "log.h"

#include <windows.h>

namespace il2cpp {

#define NL_DEFINE(ret, name, args) name##_t name = nullptr;
NL_IL2CPP_API(NL_DEFINE)
#undef NL_DEFINE

uintptr_t base = 0;

void log_exception(const void* ex, const char* where) {
    char msg[2048] = {}, stack[4096] = {};
    if (ex && format_exception) format_exception(ex, msg, sizeof msg);
    if (ex && format_stack_trace) format_stack_trace(ex, stack, sizeof stack);
    nl::logf("%s: managed exception: %s", where, msg);
    nl::logf("%s: stack:\n%s", where, stack);
}

int resolve(void* gameAssembly) {
    HMODULE ga = (HMODULE)gameAssembly;
    base = (uintptr_t)ga;
    int missing = 0;
#define NL_RESOLVE(ret, name, args)                                                  \
    name = (name##_t)GetProcAddress(ga, "il2cpp_" #name);                            \
    if (!name) {                                                                     \
        nl::logf("il2cpp: export il2cpp_%s NOT FOUND", #name);                       \
        ++missing;                                                                   \
    }
    NL_IL2CPP_API(NL_RESOLVE)
#undef NL_RESOLVE
    return missing;
}

} // namespace il2cpp
