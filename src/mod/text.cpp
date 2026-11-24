#include "text.h"
#include "log.h"
#include "unity.h"

#include <windows.h>
#include <cstring>
#include <map>
#include <tuple>

namespace nl::text {
namespace {

using Ctor_t = void (*)(void*, int, int, int, bool, bool, const MethodInfo*);  // (w, h, TextureFormat, mipChain, linear)
using GetData_t = void* (*)(void*, int, const MethodInfo*);
using Apply_t = void (*)(void*, bool, bool, const MethodInfo*);

using SetHideFlags_t = void (*)(void*, int, const MethodInfo*);

Il2CppClass* g_tex2d = nullptr;
const MethodInfo *g_ctor = nullptr, *g_getData = nullptr, *g_apply = nullptr, *g_setHideFlags = nullptr;

// UnityEngine.Object.m_CachedPtr (offset 0x10) is cleared when the native object is destroyed.
bool alive(void* obj) { return obj && *(void**)((char*)obj + 0x10) != nullptr; }
constexpr int kRGBA32 = 4;

std::map<std::tuple<std::string, int, bool>, Tex> g_cache;
Tex g_empty;

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

Tex render(const std::string& utf8, int px, bool bold) {
    std::wstring s = widen(utf8);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT rc{0, 0, 0, 0};
    DrawTextW(dc, s.c_str(), (int)s.size(), &rc, DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    int w = rc.right + 2, h = rc.bottom + 2;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT dst{1, 1, w - 1, h - 1};
    DrawTextW(dc, s.c_str(), (int)s.size(), &dst, DT_NOPREFIX | DT_SINGLELINE);
    GdiFlush();

    Tex t;
    void* tex = il2cpp::object_new(g_tex2d);
    il2cpp::gchandle_new(tex, false);  // pin at once: only native memory references it from here on
    ((Ctor_t)il2cpp::method_pointer(g_ctor))(tex, w, h, kRGBA32, false, false, g_ctor);
    auto* out = (uint8_t*)((GetData_t)il2cpp::method_pointer(g_getData))(tex, 0, g_getData);
    if (out) {
        auto* in = (const uint8_t*)bits;
        for (int y = 0; y < h; ++y) {
            const uint8_t* src = in + (size_t)y * w * 4;
            uint8_t* row = out + (size_t)(h - 1 - y) * w * 4;  // Unity rows are bottom-up
            for (int x = 0; x < w; ++x) {
                uint8_t a = src[x * 4 + 1];  // greyscale coverage (green channel)
                row[x * 4 + 0] = 255;
                row[x * 4 + 1] = 255;
                row[x * 4 + 2] = 255;
                row[x * 4 + 3] = a;
            }
        }
        static int logged = 0;
        if (logged < 5) {
            ++logged;
            uint64_t sum = 0;
            for (int i = 0; i < w * h; ++i) sum += out[i * 4 + 3];
            logf("text: rendered %dx%d px=%d alphaSum=%llu tex=%p data=%p", w, h, px, (unsigned long long)sum, tex,
                 (void*)out);
        }
        ((Apply_t)il2cpp::method_pointer(g_apply))(tex, false, false, g_apply);
        // Scene loads call Resources.UnloadUnusedAssets, which destroys script-made textures
        // nobody in the scene references. HideAndDontSave (61) keeps ours.
        if (g_setHideFlags) ((SetHideFlags_t)il2cpp::method_pointer(g_setHideFlags))(tex, 61, g_setHideFlags);
        t = {tex, w, h};
    } else {
        logf("text: GetWritableImageData returned null");
    }

    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return t;
}

} // namespace

bool init() {
    g_tex2d = unity::klass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
    g_ctor = unity::method(g_tex2d, ".ctor",
                           {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean", "System.Boolean"});
    g_getData = unity::method(g_tex2d, "GetWritableImageData", 1);
    g_apply = unity::method(g_tex2d, "Apply", {"System.Boolean", "System.Boolean"});
    g_setHideFlags = unity::method(unity::klass("UnityEngine.CoreModule.dll", "UnityEngine", "Object"), "set_hideFlags", 1);
    bool ok = g_ctor && g_getData && g_apply;
    logf("text: %s", ok ? "ready" : "Texture2D methods missing -- no text");
    return ok;
}

const Tex& get(const std::string& utf8, int px, bool bold) {
    if (!g_ctor || utf8.empty()) return g_empty;
    auto key = std::make_tuple(utf8, px, bold);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        if (alive(it->second.texture)) return it->second;
        it->second = render(utf8, px, bold);  // destroyed behind our back: redraw it
        return it->second;
    }
    if (g_cache.size() > 2000) g_cache.clear();  // crude bound; textures stay pinned, fine for menu text
    return g_cache.emplace(key, render(utf8, px, bold)).first->second;
}

} // namespace nl::text
