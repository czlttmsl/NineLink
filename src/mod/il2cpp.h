#pragma once
// Minimal view of the IL2CPP embedding API exported by GameAssembly.dll.
// Every function is resolved by name at runtime, so the mod follows game
// updates as long as the class / method names it uses stay the same.
#include <cstddef>
#include <cstdint>

// What IL2CPP throws for a managed exception (RTTI name ".?AUIl2CppExceptionWrapper@@").
struct Il2CppExceptionWrapper {
    void* ex;  // Il2CppException*
};

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppType;
struct Il2CppThread;
struct FieldInfo;
struct MethodInfo;

namespace il2cpp {

#define NL_IL2CPP_API(X)                                                                       \
    X(Il2CppDomain*, domain_get, ())                                                           \
    X(const Il2CppAssembly**, domain_get_assemblies, (const Il2CppDomain*, size_t*))           \
    X(Il2CppThread*, thread_attach, (Il2CppDomain*))                                           \
    X(void, thread_detach, (Il2CppThread*))                                                    \
    X(const Il2CppImage*, assembly_get_image, (const Il2CppAssembly*))                         \
    X(const char*, image_get_name, (const Il2CppImage*))                                       \
    X(size_t, image_get_class_count, (const Il2CppImage*))                                     \
    X(const Il2CppClass*, image_get_class, (const Il2CppImage*, size_t))                       \
    X(Il2CppClass*, class_from_name, (const Il2CppImage*, const char*, const char*))           \
    X(const char*, class_get_name, (Il2CppClass*))                                             \
    X(const char*, class_get_namespace, (Il2CppClass*))                                        \
    X(Il2CppClass*, class_get_parent, (Il2CppClass*))                                          \
    X(Il2CppClass*, class_get_declaring_type, (Il2CppClass*))                                  \
    X(FieldInfo*, class_get_fields, (Il2CppClass*, void**))                                    \
    X(const MethodInfo*, class_get_methods, (Il2CppClass*, void**))                            \
    X(const MethodInfo*, class_get_method_from_name, (Il2CppClass*, const char*, int))         \
    X(int, class_get_flags, (const Il2CppClass*))                                              \
    X(bool, class_is_valuetype, (const Il2CppClass*))                                          \
    X(bool, class_is_enum, (const Il2CppClass*))                                               \
    X(bool, class_is_interface, (const Il2CppClass*))                                          \
    X(const char*, field_get_name, (FieldInfo*))                                               \
    X(int, field_get_flags, (FieldInfo*))                                                      \
    X(size_t, field_get_offset, (FieldInfo*))                                                  \
    X(const Il2CppType*, field_get_type, (FieldInfo*))                                         \
    X(const char*, method_get_name, (const MethodInfo*))                                       \
    X(uint32_t, method_get_flags, (const MethodInfo*, uint32_t*))                              \
    X(uint32_t, method_get_param_count, (const MethodInfo*))                                   \
    X(const Il2CppType*, method_get_param, (const MethodInfo*, uint32_t))                      \
    X(const char*, method_get_param_name, (const MethodInfo*, uint32_t))                       \
    X(const Il2CppType*, method_get_return_type, (const MethodInfo*))                          \
    X(char*, type_get_name, (const Il2CppType*))                                               \
    X(void*, string_new, (const char*))                                                        \
    X(void*, object_new, (const Il2CppClass*))                                                 \
    X(void*, array_new, (Il2CppClass*, size_t))                                                \
    X(void, gc_wbarrier_set_field, (void*, void**, void*))                                     \
    X(FieldInfo*, class_get_field_from_name, (Il2CppClass*, const char*))                      \
    X(void, field_static_get_value, (FieldInfo*, void*))                                       \
    X(void, field_static_set_value, (FieldInfo*, void*))                                       \
    X(void, runtime_class_init, (Il2CppClass*))                                                \
    X(Il2CppClass*, array_class_get, (Il2CppClass*, uint32_t))                                 \
    X(void, field_set_value, (void*, FieldInfo*, void*))                                       \
    X(Il2CppClass*, object_get_class, (void*))                                                 \
    X(Il2CppClass*, class_from_type, (const Il2CppType*))                                      \
    X(const Il2CppType*, class_get_type, (Il2CppClass*))                                       \
    X(void*, type_get_object, (const Il2CppType*))                                             \
    X(void*, runtime_invoke, (const MethodInfo*, void*, void**, void**))                      \
    X(void, format_exception, (const void*, char*, int))                                       \
    X(void, format_stack_trace, (const void*, char*, int))                                     \
    X(uint16_t*, string_chars, (void*))                                                        \
    X(int32_t, string_length, (void*))                                                         \
    /* Unity 2021.2+ IL2CPP: GC handles are pointers (Il2CppGCHandle), not uint32_t. */        \
    X(void*, gchandle_new, (void*, bool))                                                      \
    X(void, gchandle_free, (void*))                                                            \
    X(void, free, (void*))

#define NL_DECLARE(ret, name, args) using name##_t = ret(*) args; extern name##_t name;
NL_IL2CPP_API(NL_DECLARE)
#undef NL_DECLARE

// GameAssembly.dll base, valid after resolve().
extern uintptr_t base;

// Logs a managed exception caught as Il2CppExceptionWrapper (message + managed stack).
void log_exception(const void* il2cppException, const char* where);

// Resolves every export. Returns the number that could not be found (0 = all good).
int resolve(void* gameAssembly);

// The compiled code of a method. In every IL2CPP version so far the method
// pointer is the first field of MethodInfo.
inline void* method_pointer(const MethodInfo* m) { return m ? *(void* const*)m : nullptr; }

} // namespace il2cpp
