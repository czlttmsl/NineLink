#include "dumper.h"
#include "il2cpp.h"
#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace nl {
namespace {

// Assemblies that get full detail. Everything else is listed by class name only.
bool is_game_assembly(const char* image) {
    static const char* full[] = {"Assembly-CSharp.dll", "Assembly-CSharp-firstpass.dll",
                                 "UnityEngine.IMGUIModule.dll", "com.rlabrecque.steamworks.net.dll",
                                 "UnityEngine.CoreModule.dll", "Newtonsoft.Json.dll"};
    for (const char* f : full)
        if (_stricmp(image, f) == 0) return true;
    return false;
}

std::string type_name(const Il2CppType* t) {
    if (!t) return "?";
    char* n = il2cpp::type_get_name(t);
    if (!n) return "?";
    std::string s = n;
    il2cpp::free(n);
    return s;
}

std::string class_full_name(Il2CppClass* k) {
    std::string name = il2cpp::class_get_name(k);
    for (Il2CppClass* d = il2cpp::class_get_declaring_type(k); d; d = il2cpp::class_get_declaring_type(d))
        name = std::string(il2cpp::class_get_name(d)) + "." + name;
    Il2CppClass* top = k;
    while (Il2CppClass* d = il2cpp::class_get_declaring_type(top)) top = d;
    const char* ns = il2cpp::class_get_namespace(top);
    if (ns && *ns) name = std::string(ns) + "." + name;
    return name;
}

const char* access(uint32_t flags) {
    switch (flags & 7) {
    case 1: return "private ";
    case 2: return "private protected ";
    case 3: return "internal ";
    case 4: return "protected ";
    case 5: return "protected internal ";
    case 6: return "public ";
    default: return "";
    }
}

// TypeAttributes visibility uses a different encoding from field/method access.
const char* type_access(uint32_t flags) {
    switch (flags & 7) {
    case 1: case 2: return "public ";
    case 3: return "private ";
    case 4: return "protected ";
    case 6: return "private protected ";
    case 7: return "protected internal ";
    default: return "internal ";
    }
}

void append(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

void dump_class_body(Il2CppClass* k, std::string& out) {
    std::string kind = il2cpp::class_is_enum(k)        ? "enum"
                       : il2cpp::class_is_interface(k) ? "interface"
                       : il2cpp::class_is_valuetype(k) ? "struct"
                                                       : "class";
    out += "\n";
    out += type_access((uint32_t)il2cpp::class_get_flags(k));
    out += kind + " " + class_full_name(k);
    if (Il2CppClass* p = il2cpp::class_get_parent(k)) {
        std::string pn = class_full_name(p);
        if (pn != "System.Object" && pn != "System.ValueType" && pn != "System.Enum") out += " : " + pn;
    }
    out += "\n{\n";

    void* it = nullptr;
    while (FieldInfo* f = il2cpp::class_get_fields(k, &it)) {
        int fl = il2cpp::field_get_flags(f);
        bool isStatic = fl & 0x10, isConst = fl & 0x40;
        append(out, "    %s%s%s %s;", access((uint32_t)fl), isConst ? "const " : isStatic ? "static " : "",
               type_name(il2cpp::field_get_type(f)).c_str(), il2cpp::field_get_name(f));
        if (!isStatic && !isConst) append(out, " // 0x%zX", il2cpp::field_get_offset(f));
        out += "\n";
    }

    it = nullptr;
    while (const MethodInfo* m = il2cpp::class_get_methods(k, &it)) {
        uint32_t iflags = 0;
        uint32_t fl = il2cpp::method_get_flags(m, &iflags);
        void* code = il2cpp::method_pointer(m);
        if (code)
            append(out, "    // RVA 0x%llX\n", (unsigned long long)((uintptr_t)code - il2cpp::base));
        out += "    ";
        out += access(fl);
        if (fl & 0x10) out += "static ";
        else if (fl & 0x400) out += "abstract ";
        else if (fl & 0x40) out += "virtual ";
        out += type_name(il2cpp::method_get_return_type(m)) + " " + il2cpp::method_get_name(m) + "(";
        uint32_t n = il2cpp::method_get_param_count(m);
        for (uint32_t i = 0; i < n; ++i) {
            if (i) out += ", ";
            const char* pn = il2cpp::method_get_param_name(m, i);
            out += type_name(il2cpp::method_get_param(m, i)) + " " + (pn ? pn : "p");
        }
        out += ");\n";
    }
    out += "}\n";
}

// SEH barrier: one broken class must not take the game down with it.
bool dump_class_safe(Il2CppClass* k, std::string& out) {
    __try {
        dump_class_body(k, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool dump_types(const std::wstring& path) {
    size_t count = 0;
    const Il2CppAssembly** asms = il2cpp::domain_get_assemblies(il2cpp::domain_get(), &count);
    if (!asms || !count) {
        logf("dump: no assemblies");
        return false;
    }

    std::string out;
    out.reserve(32 << 20);
    append(out, "// NineLink type dump. GameAssembly.dll base %p. %zu assemblies.\n", (void*)il2cpp::base, count);

    size_t totalClasses = 0, failed = 0;
    for (size_t a = 0; a < count; ++a) {
        const Il2CppImage* img = il2cpp::assembly_get_image(asms[a]);
        const char* iname = il2cpp::image_get_name(img);
        size_t n = il2cpp::image_get_class_count(img);
        bool full = is_game_assembly(iname);
        append(out, "\n// ======== %s (%zu classes)%s ========\n", iname, n, full ? "" : " -- names only");
        for (size_t c = 0; c < n; ++c) {
            Il2CppClass* k = (Il2CppClass*)il2cpp::image_get_class(img, c);
            if (!k) continue;
            ++totalClasses;
            if (full) {
                if (!dump_class_safe(k, out)) {
                    ++failed;
                    append(out, "\n// !! class #%zu could not be read\n", c);
                }
            } else {
                out += "//   " + class_full_name(k) + "\n";
            }
        }
        logf("dump: %-50s %6zu classes%s", iname, n, full ? " (full)" : "");
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        logf("dump: cannot write %s", narrow(path).c_str());
        return false;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    logf("dump: %zu classes, %zu unreadable, %zu KB -> %s", totalClasses, failed, out.size() / 1024,
         narrow(path).c_str());
    return true;
}

} // namespace nl
