#include "gui.h"
#include "log.h"

#include <windows.h>
#include <vector>

namespace nl::gui {
namespace {

using DrawTex_t = void (*)(Rect, void*, const MethodInfo*);
using SetColor_t = void (*)(Color, const MethodInfo*);
using GetObj_t = void* (*)(const MethodInfo*);
using GetInt_t = int (*)(const MethodInfo*);

struct Fn {
    const MethodInfo* m = nullptr;
    template <class T> T get() const { return (T)il2cpp::method_pointer(m); }
};
Fn g_drawTex, g_setColor, g_white, g_screenW, g_screenH;
void* g_whiteTex = nullptr;

HWND g_wnd = nullptr;
POINT g_mouse{};
bool g_down = false, g_prevDown = false, g_rdown = false, g_prevRdown = false;
bool g_press = false, g_release = false, g_rrelease = false;

BOOL CALLBACK FindGameWindow(HWND w, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameW(w, cls, 64) && wcscmp(cls, L"UnityWndClass") == 0) {
        g_wnd = w;
        return FALSE;
    }
    return TRUE;
}

std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

} // namespace

float U = 1.0f, W = 1920, H = 1080;

bool init() {
    Il2CppClass* gui = unity::klass("UnityEngine.IMGUIModule.dll", "UnityEngine", "GUI");
    Il2CppClass* screen = unity::klass("UnityEngine.CoreModule.dll", "UnityEngine", "Screen");
    Il2CppClass* tex2d = unity::klass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D");
    g_drawTex.m = unity::method(gui, "DrawTexture", {"UnityEngine.Rect", "UnityEngine.Texture"});
    g_setColor.m = unity::method(gui, "set_color", 1);
    g_white.m = unity::method(tex2d, "get_whiteTexture", 0);
    g_screenW.m = unity::method(screen, "get_width", 0);
    g_screenH.m = unity::method(screen, "get_height", 0);
    for (Fn* f : {&g_drawTex, &g_setColor, &g_white, &g_screenW, &g_screenH})
        if (!f->m || !il2cpp::method_pointer(f->m)) return false;
    return text::init();
}

void begin_frame() {
    W = (float)g_screenW.get<GetInt_t>()(g_screenW.m);
    H = (float)g_screenH.get<GetInt_t>()(g_screenH.m);
    U = H / 1080.0f;
    g_whiteTex = g_white.get<GetObj_t>()(g_white.m);  // not cached: Unity may recreate it

    if (!g_wnd) EnumWindows(FindGameWindow, 0);
    GetCursorPos(&g_mouse);
    if (g_wnd) ScreenToClient(g_wnd, &g_mouse);
    bool focused = GetForegroundWindow() == g_wnd;
    g_down = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    g_rdown = focused && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    // OnGUI runs several times per frame; edges fire on the first call that sees them.
    g_press = !g_prevDown && g_down;
    g_release = g_prevDown && !g_down;
    g_rrelease = g_prevRdown && !g_rdown;
    g_prevDown = g_down;
    g_prevRdown = g_rdown;
}

void end_frame() {
    g_press = g_release = g_rrelease = false;
    SetColor(kWhite);
}

void SetColor(Color c) { g_setColor.get<SetColor_t>()(c, g_setColor.m); }

void Fill(Rect r, Color c) {
    SetColor(c);
    g_drawTex.get<DrawTex_t>()(r, g_whiteTex, g_drawTex.m);
}

void Frame(Rect r, Color fill, Color edge, float t) {
    Fill(r, edge);
    Fill({r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t}, fill);
}

void Image(Rect r, void* tex, Color tint) {
    if (!tex) return;
    SetColor(tint);
    g_drawTex.get<DrawTex_t>()(r, tex, g_drawTex.m);
}

void Text(float x, float y, float lineH, const std::string& s, float size, Color c, Align a, bool bold) {
    const text::Tex& t = text::get(s, (int)(size * U), bold);
    if (!t.texture) return;
    float tx = a == Left ? x : a == Center ? x - t.w / 2.0f : x - t.w;
    float ty = y + (lineH - t.h) / 2.0f;
    Image({tx, ty, (float)t.w, (float)t.h}, t.texture, c);
}

void TextBox(Rect r, const std::string& s, float size, Color c, Align a) {
    // Greedy wrap: measure growing prefixes with the text renderer (cached), break when too wide.
    std::vector<std::string> chars = utf8_chars(s), lines;
    std::string line;
    for (const std::string& ch : chars) {
        std::string next = line + ch;
        const text::Tex& t = text::get(next, (int)(size * U));
        if (t.w > r.w && !line.empty()) {
            lines.push_back(line);
            line = ch;
        } else {
            line = next;
        }
    }
    if (!line.empty()) lines.push_back(line);
    float lh = size * U * 1.45f, y = r.y;
    for (const std::string& l : lines) {
        float x = a == Left ? r.x : a == Center ? r.x + r.w / 2 : r.x + r.w;
        Text(x, y, lh, l, size, c, a);
        y += lh;
        if (y > r.y + r.h) break;
    }
}

bool Button(Rect r, const std::string& label, bool enabled, bool on, float size) {
    bool hot = enabled && Inside(r);
    Color fill = !enabled ? kButtonOff : on ? kButtonOn : hot ? kButtonHot : kButton;
    Frame(r, fill, hot || on ? kGold : Color{0.45f, 0.35f, 0.22f, 1}, 2 * U);
    Text(r.x + r.w / 2, r.y, r.h, label, size, enabled ? (on ? Color{0.12f, 0.08f, 0.04f, 1} : kText) : kTextDim,
         Center, true);
    return enabled && TakeClick(r);
}

bool Inside(Rect r) {
    return g_mouse.x >= r.x && g_mouse.x < r.x + r.w && g_mouse.y >= r.y && g_mouse.y < r.y + r.h;
}
float MouseX() { return (float)g_mouse.x; }
float MouseY() { return (float)g_mouse.y; }
bool MouseDown() { return g_down; }
bool Pressed() { return g_press; }
bool Released() { return g_release; }

bool TakePress(Rect r) {
    if (g_press && Inside(r)) {
        g_press = false;
        return true;
    }
    return false;
}

bool TakeClick(Rect r) {
    if (g_release && Inside(r)) {
        g_release = false;
        return true;
    }
    return false;
}

bool RightClick(Rect r) {
    if (g_rrelease && Inside(r)) {
        g_rrelease = false;
        return true;
    }
    return false;
}

} // namespace nl::gui
