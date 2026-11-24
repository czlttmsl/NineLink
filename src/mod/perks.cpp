#include "perks.h"
#include "lobby.h"
#include "log.h"
#include "run.h"
#include "unity.h"

#include <cstring>
#include <map>

namespace nl::perks {
namespace {

using unity::Rect;
using GetObj_t = void* (*)(const MethodInfo*);
using Inst_t = void* (*)(void*, const MethodInfo*);
using InstInt_t = void* (*)(void*, int, const MethodInfo*);
using InstBool_t = bool (*)(void*, const MethodInfo*);
using GetRect_t = Rect (*)(void*, const MethodInfo*);
using GetFormat_t = int (*)(void*, const MethodInfo*);
using Ctor_t = void (*)(void*, int, int, int, bool, bool, const MethodInfo*);
using Copy_t = void (*)(void*, int, int, int, int, int, int, void*, int, int, int, int, const MethodInfo*);
using SetInt_t = void (*)(void*, int, const MethodInfo*);

const MethodInfo *g_getPerks, *g_groupArray, *g_kingSpecific, *g_generic, *g_filter, *g_locName, *g_locDesc,
    *g_term, *g_universal, *g_spriteTex, *g_spriteRect, *g_texFormat, *g_texCtor, *g_copy, *g_filterMode,
    *g_hideFlags;
Il2CppClass *g_mco = nullptr, *g_tex2d = nullptr;

std::vector<Perk*> g_all;
std::map<std::string, Perk*> g_byTerm;
std::map<int, std::vector<Category>> g_cats;
bool g_loaded = false;

template <class T> T fn(const MethodInfo* m) { return (T)il2cpp::method_pointer(m); }

int mco_value(const char* name) {
    FieldInfo* f = g_mco ? il2cpp::class_get_field_from_name(g_mco, name) : nullptr;
    int v = 0;
    if (f) il2cpp::field_static_get_value(f, &v);
    return v;
}

const char* kKingNames[lobby::kKingCount] = {"KingOfNothing",  "KingOfSpells", "KingOfBlood",
                                             "KingOfGreed",    "KingOfProgress", "KingOfStone",
                                             "KingOfNomads",   "KingOfNature", "KingOfTime"};

std::string strip_tags(std::string s) {
    std::string out;
    bool tag = false;
    for (char c : s) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag && c != '\r') out += c == '\n' ? ' ' : c;
    }
    return out;
}

// IL2CPP containers: List<T> { T[] _items @0x10; int _size @0x18 }, T[] elements start at 0x20.
template <class F> void each_in_list(void* list, F f) {
    if (!list) return;
    void* items = *(void**)((char*)list + 0x10);
    int size = *(int*)((char*)list + 0x18);
    for (int i = 0; items && i < size; ++i) f(((void**)((char*)items + 0x20))[i]);
}
template <class F> void each_in_array(void* arr, F f) {
    if (!arr) return;
    size_t n = *(size_t*)((char*)arr + 0x18);
    for (size_t i = 0; i < n; ++i) f(((void**)((char*)arr + 0x20))[i]);
}

void* game_perks() { return fn<GetObj_t>(g_getPerks)(g_getPerks); }

Perk* wrap(void* obj) {
    for (Perk* p : g_all)
        if (p->obj == obj) return p;
    auto* p = new Perk;
    p->obj = obj;
    p->cls = il2cpp::class_get_name(il2cpp::object_get_class(obj));
    // MultiplayerHubPerkSlotsManager.CalculatePerks stores Perk.Name (+0x20) as perkTerm.
    p->term = run::to_utf8(*(void**)((char*)obj + 0x20));
    p->name = strip_tags(run::to_utf8(fn<Inst_t>(g_locName)(obj, g_locName)));
    p->desc = strip_tags(run::to_utf8(fn<Inst_t>(g_locDesc)(obj, g_locDesc)));
    p->sprite = *(void**)((char*)obj + 0x18);  // Perk.Sprite
    p->maxLevel = *(int*)((char*)obj + 0x30);  // Perk.MaxLevel
    memcpy(p->theme, (char*)obj + 0x34, sizeof p->theme);  // Perk.Theme
    p->owner = *(int*)((char*)obj + 0x44);     // Perk.Owner
    p->universal = fn<InstBool_t>(g_universal)(obj, g_universal);
    g_all.push_back(p);
    g_byTerm[p->term] = p;
    return p;
}

void load_all() {
    if (g_loaded) return;
    g_loaded = true;
    void* gp = game_perks();
    if (!gp) {
        logf("perks: Core.Game.Perks is null");
        return;
    }
    each_in_array(fn<Inst_t>(g_groupArray)(gp, g_groupArray), [](void* o) { if (o) wrap(o); });
    logf("perks: %zu perks in the catalogue", g_all.size());
}

} // namespace

bool init() {
    const char* game = "Assembly-CSharp.dll";
    const char* core = "UnityEngine.CoreModule.dll";
    g_getPerks = unity::method(unity::klass(game, "Core", "Game"), "get_Perks", 0);
    Il2CppClass* gp = unity::klass(game, "PerkSystem", "GamePerks");
    g_groupArray = unity::method(gp, "get_GroupArray", 0);
    g_kingSpecific = unity::method(gp, "GetKingSpecificPerks", 1);
    g_generic = unity::method(gp, "GetGenericPerks", 0);
    g_filter = unity::method(gp, "FilterByKingOwnership", 1);
    Il2CppClass* perk = unity::klass(game, "PerkSystem", "Perk");
    g_locName = unity::method(perk, "get_localizedName", 0);
    g_locDesc = unity::method(perk, "get_localizedDescription", 0);
    g_term = unity::method(perk, "get_translationTerm", 0);
    g_universal = unity::method(perk, "get_IsUniversal", 0);
    g_mco = unity::klass(game, "Core", "MultipleCardOwnership");
    Il2CppClass* sprite = unity::klass(core, "UnityEngine", "Sprite");
    g_spriteTex = unity::method(sprite, "get_texture", 0);
    g_spriteRect = unity::method(sprite, "get_textureRect", 0);
    g_tex2d = unity::klass(core, "UnityEngine", "Texture2D");
    g_texFormat = unity::method(g_tex2d, "get_format", 0);
    g_texCtor = unity::method(g_tex2d, ".ctor",
                              {"System.Int32", "System.Int32", "UnityEngine.TextureFormat", "System.Boolean", "System.Boolean"});
    g_copy = unity::method(unity::klass(core, "UnityEngine", "Graphics"), "CopyTexture",
                           {"UnityEngine.Texture", "System.Int32", "System.Int32", "System.Int32", "System.Int32",
                            "System.Int32", "System.Int32", "UnityEngine.Texture", "System.Int32", "System.Int32",
                            "System.Int32", "System.Int32"});
    g_filterMode = unity::method(unity::klass(core, "UnityEngine", "Texture"), "set_filterMode", 1);
    g_hideFlags = unity::method(unity::klass(core, "UnityEngine", "Object"), "set_hideFlags", 1);
    bool ok = g_getPerks && g_groupArray && g_kingSpecific && g_generic && g_locName && g_locDesc && g_term;
    logf("perks: %s (icons %s)", ok ? "ready" : "missing methods", g_copy && g_spriteRect ? "ok" : "unavailable");
    return ok;
}

const std::vector<Category>& categories(int k) {
    static const std::vector<Category> none;
    if (k < 0 || k >= lobby::kKingCount) return none;
    auto it = g_cats.find(k);
    if (it != g_cats.end()) return it->second;
    load_all();
    std::vector<Category>& cats = g_cats[k];
    int kingBit = mco_value(kKingNames[k]);
    Category generic{"通用特权", {}}, king{std::string(lobby::king_name(k)) + "特权", {}}, multi{"多人特权", {}};
    for (Perk* p : g_all) {
        if (p->owner == kGenericOwner) generic.perks.push_back(p);
        else if (p->owner == kingBit) king.perks.push_back(p);
        else if (p->owner == kMultiplayerOwner && p->term != "Perk_Ambitious" && p->term != "Perk_Vain")
            multi.perks.push_back(p);  // ranking points / medals mean nothing outside official ranked play
    }
    for (Category* c : {&king, &generic, &multi})
        if (!c->perks.empty()) cats.push_back(*c);
    logf("perks: %s -> king %zu, generic %zu, multiplayer %zu", kKingNames[k], king.perks.size(),
         generic.perks.size(), multi.perks.size());
    return cats;
}

Perk* by_term(const std::string& term) {
    load_all();
    auto it = g_byTerm.find(term);
    return it == g_byTerm.end() ? nullptr : it->second;
}

const text::Tex& icon(Perk* p) {
    if (p->icon.texture && *(void**)((char*)p->icon.texture + 0x10)) return p->icon;  // still alive
    p->icon = {};
    if (!p->sprite || !g_copy || !g_spriteRect || !g_texCtor) return p->icon;
    void* src = fn<Inst_t>(g_spriteTex)(p->sprite, g_spriteTex);
    if (!src) return p->icon;
    Rect r = fn<GetRect_t>(g_spriteRect)(p->sprite, g_spriteRect);
    int x = (int)r.x, y = (int)r.y, w = (int)r.w, h = (int)r.h;
    if (w <= 0 || h <= 0) return p->icon;
    int format = fn<GetFormat_t>(g_texFormat)(src, g_texFormat);
    void* dst = il2cpp::object_new(g_tex2d);
    il2cpp::gchandle_new(dst, false);  // pin at once: only native memory references it from here on
    fn<Ctor_t>(g_texCtor)(dst, w, h, format, false, false, g_texCtor);
    // GPU-side copy: works for non-readable atlases. Do NOT Apply() dst afterwards --
    // that would upload its (empty) CPU copy over the pixels we just copied.
    fn<Copy_t>(g_copy)(src, 0, 0, x, y, w, h, dst, 0, 0, 0, 0, g_copy);
    if (g_filterMode) fn<SetInt_t>(g_filterMode)(dst, 0, g_filterMode);  // point filtering: crisp pixel art
    if (g_hideFlags) fn<SetInt_t>(g_hideFlags)(dst, 61, g_hideFlags);
    p->icon = {dst, w, h};
    return p->icon;
}

void dump_catalog() {
    load_all();
    for (Perk* p : g_all)
        logf("perk: %-14s owner=0x%04X max=%d term=%s | %s | %s", p->cls.c_str(), p->owner, p->maxLevel,
             p->term.c_str(), p->name.c_str(), p->desc.c_str());
    for (int k = 0; k < lobby::kKingCount; ++k) categories(k);
}

} // namespace nl::perks
