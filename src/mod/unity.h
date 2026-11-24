#pragma once
// Helpers for finding game / engine code by name through the IL2CPP API.
#include "il2cpp.h"

#include <initializer_list>
#include <string>

namespace nl::unity {

const Il2CppImage* image(const char* name);  // e.g. "Assembly-CSharp.dll"
Il2CppClass* klass(const char* image, const char* ns, const char* name);

// Finds a method by name and exact parameter type names, e.g.
// method(GUI, "Button", {"UnityEngine.Rect", "System.String"}).
const MethodInfo* method(Il2CppClass* k, const char* name, std::initializer_list<const char*> params);
// Any overload with `argc` parameters.
const MethodInfo* method(Il2CppClass* k, const char* name, int argc);

// Replaces a method's code pointer in its MethodInfo. Unity invokes its
// messages (Update, OnGUI, ...) through the MethodInfo, so this reroutes them
// without patching any machine code. Returns the original pointer.
void* swap_method_pointer(const MethodInfo* m, void* replacement);

inline void* str(const char* utf8) { return il2cpp::string_new(utf8); }

struct Rect {
    float x, y, w, h;
};

} // namespace nl::unity
