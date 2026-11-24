#include "unity.h"
#include "log.h"

#include <windows.h>
#include <cstring>

namespace nl::unity {

const Il2CppImage* image(const char* name) {
    size_t n = 0;
    const Il2CppAssembly** asms = il2cpp::domain_get_assemblies(il2cpp::domain_get(), &n);
    for (size_t i = 0; i < n; ++i) {
        const Il2CppImage* img = il2cpp::assembly_get_image(asms[i]);
        if (_stricmp(il2cpp::image_get_name(img), name) == 0) return img;
    }
    logf("unity: image %s not found", name);
    return nullptr;
}

Il2CppClass* klass(const char* img, const char* ns, const char* name) {
    const Il2CppImage* i = image(img);
    Il2CppClass* k = i ? il2cpp::class_from_name(i, ns, name) : nullptr;
    if (!k) logf("unity: class %s.%s not found in %s", ns, name, img);
    return k;
}

static bool param_is(const MethodInfo* m, uint32_t i, const char* want) {
    char* n = il2cpp::type_get_name(il2cpp::method_get_param(m, i));
    bool ok = n && strcmp(n, want) == 0;
    if (n) il2cpp::free(n);
    return ok;
}

const MethodInfo* method(Il2CppClass* k, const char* name, std::initializer_list<const char*> params) {
    if (!k) return nullptr;
    void* it = nullptr;
    while (const MethodInfo* m = il2cpp::class_get_methods(k, &it)) {
        if (strcmp(il2cpp::method_get_name(m), name) != 0) continue;
        if (il2cpp::method_get_param_count(m) != params.size()) continue;
        uint32_t i = 0;
        bool ok = true;
        for (const char* p : params)
            if (!param_is(m, i++, p)) { ok = false; break; }
        if (ok) return m;
    }
    logf("unity: method %s.%s(%zu params) not found", il2cpp::class_get_name(k), name, params.size());
    return nullptr;
}

const MethodInfo* method(Il2CppClass* k, const char* name, int argc) {
    if (!k) return nullptr;
    const MethodInfo* m = il2cpp::class_get_method_from_name(k, name, argc);
    if (!m) logf("unity: method %s.%s/%d not found", il2cpp::class_get_name(k), name, argc);
    return m;
}

void* swap_method_pointer(const MethodInfo* m, void* replacement) {
    void** slot = (void**)m;
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    void* orig = InterlockedExchangePointer(slot, replacement);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return orig;
}

} // namespace nl::unity
