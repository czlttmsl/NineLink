#include "snapshot.h"
#include "log.h"
#include "patch.h"
#include "run.h"
#include "unity.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>

namespace nl::snapshot {
namespace {

using Capture_t = void* (*)(const MethodInfo*);
using Serialize_t = void* (*)(void* value, void* settings, const MethodInfo*);
using Deserialize_t = void* (*)(void* json, void* type, void* settings, const MethodInfo*);

const MethodInfo *g_capture = nullptr, *g_serialize = nullptr, *g_deserialize = nullptr;
FieldInfo* g_settingsField = nullptr;
Il2CppClass* g_snapshotClass = nullptr;
std::map<void*, void*> g_pinned;  // object -> GC handle

int filter(EXCEPTION_POINTERS* ep, const char* what);

// Reading Multiplayer.m_SerializerSettings runs that class's static constructor the first
// time, which may throw (it belongs to the official online mode) -- so it is guarded too.
void* seh_settings() {
    void* s = nullptr;
    __try { il2cpp::field_static_get_value(g_settingsField, &s); }
    __except (filter(GetExceptionInformation(), "Multiplayer.m_SerializerSettings")) { s = nullptr; }
    return s;
}

void* settings() {
    static bool tried = false;
    static void* cached = nullptr;
    if (!tried && g_settingsField) {
        tried = true;
        cached = seh_settings();
        if (cached) il2cpp::gchandle_new(cached, false);
        logf("snapshot: serializer settings %p (%s)", cached, cached ? "official multiplayer settings" : "Newtonsoft defaults");
    }
    return cached;  // null is fine too: Newtonsoft defaults
}

// SEH barriers (no C++ objects inside): a native fault in game code must not take the game down.
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

// An Il2CppObject* whose class name ends in "Exception"?
bool looks_like_exception(void* obj) {
    if (!readable(obj, 16)) return false;
    void* klass = *(void**)obj;
    if (!readable(klass, 32)) return false;
    const char* name = il2cpp::class_get_name((Il2CppClass*)klass);
    if (!name || !readable(name, 1)) return false;
    size_t len = strlen(name);
    return len >= 9 && strcmp(name + len - 9, "Exception") == 0;
}

int filter(EXCEPTION_POINTERS* ep, const char* what) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode == 0xE06D7363 && r->NumberParameters >= 4) {
        // MSVC C++ throw: [1] = thrown object, [2] = ThrowInfo (RVAs), [3] = image base.
        uintptr_t img = r->ExceptionInformation[3];
        const int* ti = (const int*)r->ExceptionInformation[2];
        if (readable(ti, 16) && img) {
            const int* cta = (const int*)(img + ti[3]);  // CatchableTypeArray
            if (readable(cta, 8)) {
                const int* ct = (const int*)(img + cta[1]);  // first CatchableType
                if (readable(ct, 8)) {
                    const char* tname = (const char*)(img + ct[1]) + 16;  // TypeDescriptor.name
                    if (readable(tname, 4)) logf("snapshot: %s: C++ exception of type %s", what, tname);
                }
            }
        }
        void* p = (void*)r->ExceptionInformation[1];
        void* first = readable(p, 8) ? *(void**)p : nullptr;
        void* second = readable(first, 8) ? *(void**)first : nullptr;
        if (looks_like_exception(first)) il2cpp::log_exception(first, what);
        else if (looks_like_exception(second)) il2cpp::log_exception(second, what);
        else logf("snapshot: %s: thrown object not recognised (%p -> %p -> %p)", what, p, first, second);
        return EXCEPTION_EXECUTE_HANDLER;  // swallow: this call simply failed
    }
    logf("snapshot: %s: native exception 0x%08lX at GameAssembly+0x%llX", what, r->ExceptionCode,
         (unsigned long long)((uintptr_t)r->ExceptionAddress - il2cpp::base));
    return EXCEPTION_EXECUTE_HANDLER;
}
void* seh_capture() {
    __try { return ((Capture_t)il2cpp::method_pointer(g_capture))(g_capture); }
    __except (filter(GetExceptionInformation(), "CaptureSnapshot")) { return nullptr; }
}
void* seh_serialize(void* obj, void* st) {
    __try { return ((Serialize_t)il2cpp::method_pointer(g_serialize))(obj, st, g_serialize); }
    __except (filter(GetExceptionInformation(), "SerializeObject")) { return nullptr; }
}
bool seh_void(void (*f)(const MethodInfo*), const MethodInfo* m, const char* what) {
    __try { f(m); return true; }
    __except (filter(GetExceptionInformation(), what)) { return false; }
}
void* seh_deserialize(void* json, void* type, void* st) {
    __try { return ((Deserialize_t)il2cpp::method_pointer(g_deserialize))(json, type, st, g_deserialize); }
    __except (filter(GetExceptionInformation(), "DeserializeObject")) { return nullptr; }
}

} // namespace

bool init() {
    g_snapshotClass = unity::klass("Assembly-CSharp.dll", "Gameplay", "GameSnapshot");
    g_capture = unity::method(g_snapshotClass, "CaptureSnapshot", 0);
    Il2CppClass* jc = unity::klass("Newtonsoft.Json.dll", "Newtonsoft.Json", "JsonConvert");
    g_serialize = unity::method(jc, "SerializeObject", {"System.Object", "Newtonsoft.Json.JsonSerializerSettings"});
    g_deserialize = unity::method(jc, "DeserializeObject", {"System.String", "System.Type", "Newtonsoft.Json.JsonSerializerSettings"});
    g_settingsField = il2cpp::class_get_field_from_name(
        unity::klass("Assembly-CSharp.dll", "MultiplayerSystems", "Multiplayer"), "m_SerializerSettings");
    bool ok = g_capture && g_serialize && g_deserialize;
    logf("snapshot: %s", ok ? "ready" : "missing methods");
    return ok;
}

std::string capture_json() {
    if (!g_capture || !g_serialize) return {};
    try {
        void* st = settings();
        void* snap = seh_capture();
        logf("snapshot: CaptureSnapshot -> %p", snap);
        if (!snap) return {};
        void* h = il2cpp::gchandle_new(snap, false);  // pinned while we serialise it
        void* str = seh_serialize(snap, st);
        logf("snapshot: SerializeObject -> %p (readable: %d)", str, readable(str, 24));
        il2cpp::gchandle_free(h);
        logf("snapshot: handle freed");
        if (!str || !readable(str, 24)) return {};
        void* klass = *(void**)str;
        logf("snapshot: klass ptr %p (readable: %d)", klass, readable(klass, 32));
        if (!readable(klass, 32)) return {};
        Il2CppClass* sk = (Il2CppClass*)klass;
        int len = il2cpp::string_length(str);
        logf("snapshot: result is %s.%s, %d UTF-16 chars", il2cpp::class_get_namespace(sk), il2cpp::class_get_name(sk), len);
        std::string out = len > 0 ? run::to_utf8(str) : std::string();
        logf("snapshot: %zu bytes of UTF-8", out.size());
        return out;
    } catch (const Il2CppExceptionWrapper& e) {
        il2cpp::log_exception(e.ex, "snapshot capture");
        return {};
    }
}

void* from_json(const std::string& json) {
    if (!g_deserialize || json.empty()) return nullptr;
    try {
        void* str = unity::str(json.c_str());
        void* hs = il2cpp::gchandle_new(str, false);
        void* type = il2cpp::type_get_object(il2cpp::class_get_type(g_snapshotClass));
        void* obj = seh_deserialize(str, type, settings());
        il2cpp::gchandle_free(hs);
        if (obj) g_pinned[obj] = il2cpp::gchandle_new(obj, false);
        return obj;
    } catch (const Il2CppExceptionWrapper& e) {
        il2cpp::log_exception(e.ex, "snapshot from_json");
        return nullptr;
    }
}

void* deserialize(const std::string& json, Il2CppClass* k, bool asArray) {
    if (!g_deserialize || json.empty() || !k) return nullptr;
    try {
        void* str = unity::str(json.c_str());
        void* hs = il2cpp::gchandle_new(str, false);
        Il2CppClass* target = asArray ? il2cpp::array_class_get(k, 1) : k;
        void* type = il2cpp::type_get_object(il2cpp::class_get_type(target));
        void* obj = seh_deserialize(str, type, settings());
        il2cpp::gchandle_free(hs);
        if (obj) g_pinned[obj] = il2cpp::gchandle_new(obj, false);
        return obj;
    } catch (const Il2CppExceptionWrapper& e) {
        il2cpp::log_exception(e.ex, "snapshot deserialize");
        return nullptr;
    }
}

namespace {
using SessionCtor_t = void (*)(void* self, void* data, void* pool, void* poolFallback, void* decrees, void* decreesFallback,
                               const MethodInfo*);
// Season pools must not be null: MultiplayerSession.CanCreateSeasonItem looks season cards
// (owner 0x800) up in a HashSet built from them, and a null set throws while the hand is being
// dealt -- leaving the player with no cards. Empty pools simply mean "no season cards".
void seh_session_ctor(SessionCtor_t f, void* self, void* data, void* emptyPool, const MethodInfo* m) {
    __try { f(self, data, emptyPool, emptyPool, emptyPool, emptyPool, m); }
    __except (filter(GetExceptionInformation(), "MultiplayerSession..ctor")) {}
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}
} // namespace

bool install_session(const std::vector<Opponent>& opponents) {
    const char* game = "Assembly-CSharp.dll";
    Il2CppClass* runData = unity::klass(game, "MultiplayerSystems", "MultiplayerRunData");
    Il2CppClass* sessionClass = unity::klass(game, "MultiplayerSystems", "MultiplayerSession");
    const MethodInfo* ctor = unity::method(sessionClass, ".ctor", 5);
    FieldInfo* sessionField = il2cpp::class_get_field_from_name(
        unity::klass(game, "MultiplayerSystems", "MultiplayerManager"), "Session");
    if (!runData || !sessionClass || !ctor || !sessionField) return false;

    // The opponents as MatchmakingResponse-style run records; Newtonsoft builds the objects.
    std::string json = "[";
    for (size_t i = 0; i < opponents.size(); ++i) {
        const Opponent& o = opponents[i];
        std::string snaps;
        for (const std::string& s : o.snapshots) snaps += (snaps.empty() ? "" : ",") + s;
        const std::string& last = o.snapshots.empty() ? std::string("null") : o.snapshots.back();
        char head[512];
        snprintf(head, sizeof head,
                 "{\"runId\":%d,\"playerId\":%d,\"yearPlayed\":%d,\"finalPosition\":1,\"scoreChange\":0,"
                 "\"rankPoints\":0,\"kingId\":%d,\"currentHP\":3,\"createdAt\":\"2026-01-01T00:00:00+00:00\","
                 "\"gameVersion\":\"9Kings 1.0.4\",\"troopsKilled\":0,\"runStrength\":1,",
                 o.playerId, o.playerId, o.yearPlayed, o.kingOwner);
        json += (i ? "," : "") + std::string(head) + "\"player\":{\"playerId\":" + std::to_string(o.playerId) +
                ",\"username\":\"" + json_escape(o.name) + "\"},\"snapshot\":{\"finalWaveTroopsKilled\":0," +
                "\"finalWaveTotalScaledTime\":0,\"Perks\":[],\"FinalSnapshot\":" + last + ",\"snapshots\":[" + snaps + "]}}";
    }
    json += "]";

    void* data = deserialize(json, runData, true);
    logf("snapshot: %zu opponent run(s) -> MultiplayerRunData[] %p (%zu bytes of JSON)", opponents.size(), data, json.size());
    if (!data) return false;
    void* session = il2cpp::object_new(sessionClass);
    g_pinned[session] = il2cpp::gchandle_new(session, false);
    void* emptyPool = il2cpp::array_new(unity::klass("mscorlib.dll", "System", "String"), 0);
    g_pinned[emptyPool] = il2cpp::gchandle_new(emptyPool, false);
    seh_session_ctor((SessionCtor_t)il2cpp::method_pointer(ctor), session, data, emptyPool, ctor);
    // Run MultiplayerManager's static constructor first: otherwise it runs later, on the game's
    // first use, and resets Session to null (and before it the static storage is not even set up).
    Il2CppClass* mm = unity::klass(game, "MultiplayerSystems", "MultiplayerManager");
    il2cpp::runtime_class_init(mm);
    // Write the static the way game code does: Il2CppClass+0xB8 holds the class's static data
    // block, the field lives at its offset in there. (il2cpp_field_static_set_value did not
    // take effect for this field.) Statics are GC roots, so a plain store is safe.
    char* statics = *(char**)((char*)mm + 0xB8);
    size_t off = il2cpp::field_get_offset(sessionField);
    logf("snapshot: MultiplayerManager statics %p, Session at +0x%zX", statics, off);
    if (!statics || off > 0x1000) return false;
    *(void**)(statics + off) = session;
    void* check = *(void**)(statics + off);
    void* dataCheck = *(void**)((char*)session + 0x10);  // MultiplayerSession.Data
    logf("snapshot: MultiplayerManager.Session = %p (reads back %p), Session.Data = %p", session, check, dataCheck);
    return check == session && dataCheck;
}

void log_mp_state() {
    const char* game = "Assembly-CSharp.dll";
    Il2CppClass* wave = unity::klass(game, "Core", "Wave");
    void *enemySnap = nullptr, *data = nullptr, *session = nullptr;
    il2cpp::field_static_get_value(il2cpp::class_get_field_from_name(wave, "CurrentMultiplayerEnemySnapshot"), &enemySnap);
    il2cpp::field_static_get_value(il2cpp::class_get_field_from_name(wave, "Data"), &data);
    {
        Il2CppClass* mm = unity::klass(game, "MultiplayerSystems", "MultiplayerManager");
        char* statics = *(char**)((char*)mm + 0xB8);
        size_t off = il2cpp::field_get_offset(il2cpp::class_get_field_from_name(mm, "Session"));
        session = statics ? *(void**)(statics + off) : nullptr;
    }
    const MethodInfo* enemyBase = unity::method(wave, "get_MultiplayerEnemyBase", 0);
    void* base = enemyBase ? ((void* (*)(const MethodInfo*))il2cpp::method_pointer(enemyBase))(enemyBase) : nullptr;
    bool isMp = data && *(bool*)((char*)data + 0x1A8);  // GameplayData.<IsMultiplayer>
    const MethodInfo* getWave = unity::method(unity::klass(game, "Core", "GameplayData"), "get_Wave", 0);
    int year = data && getWave ? ((int (*)(void*, const MethodInfo*))il2cpp::method_pointer(getWave))(data, getWave) : -1;
    void* gauntlet = session ? *(void**)((char*)session + 0x40) : nullptr;  // MultiplayerSession.GauntletEnemies
    int gauntletCount = gauntlet ? *(int*)((char*)gauntlet + 0x18) : -1;
    bool gauntletActive = session && *(bool*)((char*)session + 0x50);
    logf("snapshot: MP state: IsMultiplayer=%d year=%d Session=%p gauntlet=%d active=%d CurrentMultiplayerEnemySnapshot=%p MultiplayerEnemyBase=%p",
         isMp, year, session, gauntletCount, gauntletActive, enemySnap, base);
}

// Debug: why would PopulateGauntletEnemies skip our runs? Mirror its checks and log them.
void inspect_runs(void* session) {
    void* data = session ? *(void**)((char*)session + 0x10) : nullptr;  // MultiplayerRunData[]
    size_t n = data ? *(size_t*)((char*)data + 0x18) : 0;
    const char* game = "Assembly-CSharp.dll";
    const MethodInfo* getOptions = unity::method(unity::klass(game, "Core", "Wave"), "get_Options", 0);
    void* options = getOptions ? ((void* (*)(const MethodInfo*))il2cpp::method_pointer(getOptions))(getOptions) : nullptr;
    int gauntletYear = options ? *(int*)((char*)options + 0x2B8) : -1;
    const MethodInfo* gws = unity::method(unity::klass(game, "MultiplayerSystems", "MultiplayerGameSnapshots"), "GetWaveSnapshot", 2);
    for (size_t i = 0; i < n; ++i) {
        char* run = ((char**)((char*)data + 0x20))[i];
        if (!run) { logf("snapshot: run %zu is null", i); continue; }
        int hp = *(int*)(run + 0x2C), king = *(int*)(run + 0x28);
        void* snaps = *(void**)(run + 0x50);
        void* list = snaps ? *(void**)((char*)snaps + 0x28) : nullptr;  // .snapshots
        int count = list ? *(int*)((char*)list + 0x18) : -1;
        void* ws = nullptr;
        if (snaps && gws && gauntletYear >= 0)
            ws = ((void* (*)(void*, int, bool, const MethodInfo*))il2cpp::method_pointer(gws))(snaps, gauntletYear, true, gws);
        void* waveData = ws ? *(void**)((char*)ws + 0x20) : nullptr;
        logf("snapshot: run %zu: currentHP=%d kingId=%d snapshot=%p snapshots=%d options=%p year(+0x2B8)=%d GetWaveSnapshot=%p WaveData=%p",
             i, hp, king, snaps, count, options, gauntletYear, ws, waveData);
    }
}

bool prepare_gauntlet() {
    {
        Il2CppClass* mm = unity::klass("Assembly-CSharp.dll", "MultiplayerSystems", "MultiplayerManager");
        char* statics = *(char**)((char*)mm + 0xB8);
        size_t off = il2cpp::field_get_offset(il2cpp::class_get_field_from_name(mm, "Session"));
        try { inspect_runs(statics ? *(void**)(statics + off) : nullptr); }
        catch (...) { logf("snapshot: inspect_runs threw"); }
    }
    const MethodInfo* m = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "Wave"), "PrepareMultiplayerGauntlet", 0);
    if (!m) return false;
    bool ok = seh_void((void (*)(const MethodInfo*))il2cpp::method_pointer(m), m, "PrepareMultiplayerGauntlet");
    logf("snapshot: PrepareMultiplayerGauntlet %s", ok ? "done" : "FAILED");
    log_mp_state();
    return ok;
}

namespace {
patch::Redirect g_submitBlock;
void NoSubmit(int king, const MethodInfo*) {
    logf("snapshot: SubmitMultiplayerRun(king %d) blocked -- NineLink runs never reach the official servers", king);
}
} // namespace

// Core.Wave.EndWave: in multiplayer, once exactly one opponent is still alive it jumps straight
// to the final year (official rule: "last rival standing -> final showdown"). With one friend
// that fires after the first year, so during a NineLink run that `je` is NOP'd and years go on
// normally (when no opponent is left, the game's own end-of-run path still runs).
struct BytePatch {
    uintptr_t rva;
    unsigned char expect[2], replace[2], saved[2];
    bool active;
} g_lastRivalJump{0x61D30D, {0x74, 0x0C}, {0x90, 0x90}, {}, false};

void apply_byte_patch(BytePatch& p, bool on) {
    unsigned char* at = (unsigned char*)(il2cpp::base + p.rva);
    if (on == p.active) return;
    if (on && memcmp(at, p.expect, 2) != 0) {
        logf("snapshot: EndWave bytes differ (%02X %02X) -- game updated? one-rival skip NOT disabled", at[0], at[1]);
        return;
    }
    DWORD old;
    if (!VirtualProtect(at, 2, PAGE_EXECUTE_READWRITE, &old)) return;
    if (on) {
        memcpy(p.saved, at, 2);
        memcpy(at, p.replace, 2);
    } else {
        memcpy(at, p.saved, 2);
    }
    VirtualProtect(at, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 2);
    p.active = on;
    logf("snapshot: one-rival skip to the final year %s", on ? "disabled for this run" : "restored");
}

void block_official_submit(bool block) {
    apply_byte_patch(g_lastRivalJump, block);
    if (!block) {
        if (g_submitBlock.active) logf("snapshot: official submit unblocked");
        g_submitBlock.remove();
        return;
    }
    const MethodInfo* m = unity::method(unity::klass("Assembly-CSharp.dll", "MultiplayerSystems", "MultiplayerManager"),
                                        "SubmitMultiplayerRun", 1);
    bool ok = m && g_submitBlock.install(il2cpp::method_pointer(m), (void*)&NoSubmit);
    logf("snapshot: official submit %s", ok ? "blocked for this run" : "COULD NOT BE BLOCKED");
}

namespace {
void* current_session() {
    Il2CppClass* mm = unity::klass("Assembly-CSharp.dll", "MultiplayerSystems", "MultiplayerManager");
    static size_t off = il2cpp::field_get_offset(il2cpp::class_get_field_from_name(mm, "Session"));
    char* statics = mm ? *(char**)((char*)mm + 0xB8) : nullptr;
    return statics ? *(void**)(statics + off) : nullptr;
}

void* gameplay_data() {
    static FieldInfo* f = il2cpp::class_get_field_from_name(unity::klass("Assembly-CSharp.dll", "Core", "Wave"), "Data");
    void* data = nullptr;
    if (f) il2cpp::field_static_get_value(f, &data);
    return data;
}

// Owner of Wave.WaveKing (the KingAffection the castle-destroyed handler looks the opponent up by).
int wave_king_owner() {
    static Il2CppClass* wave = unity::klass("Assembly-CSharp.dll", "Core", "Wave");
    static FieldInfo* f = il2cpp::class_get_field_from_name(wave, "WaveKing");
    static const MethodInfo* owner = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "KingAffection"), "get_Owner", 0);
    void* ka = nullptr;
    if (f) il2cpp::field_static_get_value(f, &ka);
    if (!ka || !owner) return -1;
    return ((int (*)(void*, const MethodInfo*))il2cpp::method_pointer(owner))(ka, owner);
}
} // namespace

void assign_wave_kings() {
    void* data = gameplay_data();
    void* session = current_session();
    if (!data || !session || !*(bool*)((char*)data + 0x1A8)) return;  // GameplayData.<IsMultiplayer>
    // Live opponents' kings, from the session's run records (kingId +0x28, currentHP +0x2C).
    std::vector<int> owners;
    void* runs = *(void**)((char*)session + 0x10);
    size_t n = runs ? *(size_t*)((char*)runs + 0x18) : 0;
    for (size_t i = 0; i < n; ++i) {
        char* r = ((char**)((char*)runs + 0x20))[i];
        if (r && *(int*)(r + 0x2C) > 0) owners.push_back(*(int*)(r + 0x28));
    }
    if (owners.empty()) return;
    void* list = *(void**)((char*)data + 0x30);  // GameplayData.Waves : List<WaveProperties>
    if (!list) return;
    void* items = *(void**)((char*)list + 0x10);
    int count = *(int*)((char*)list + 0x18);
    if (!items || count <= 0 || (size_t)count > *(size_t*)((char*)items + 0x18)) return;
    int changed = 0, firstWave = -1;
    for (int i = 0; i < count; ++i) {
        char* p = ((char**)((char*)items + 0x20))[i];
        if (!p) continue;
        int number = *(int*)(p + 0x10), type = *(int*)(p + 0x14);
        if (type == 5 || type == 9) continue;  // Victory / EndlessVictory: the game's gauntlet picks those
        bool has = *(bool*)(p + 0x18);
        int king = *(int*)(p + 0x1C);
        bool ok = false;
        for (int o : owners) ok |= has && king == o;
        if (ok) continue;
        // One opponent per year, rotating, so consecutive years differ when there are several
        // (as the game's own GetWaveKing avoids repeating last year's king).
        int pick = owners[(number < 0 ? 0 : number) % owners.size()];
        *(int*)(p + 0x1C) = pick;
        *(bool*)(p + 0x18) = true;
        if (!changed++) firstWave = number;
    }
    if (changed) logf("snapshot: attacking king set to an opponent for %d year(s) (first: year %d)", changed, firstWave);
}

namespace {
void seh_assign_wave_kings() {
    __try { assign_wave_kings(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("snapshot: assign_wave_kings faulted"); }
}
} // namespace

void watch_years() {
    const char* game = "Assembly-CSharp.dll";
    static Il2CppClass* wave = unity::klass(game, "Core", "Wave");
    static const MethodInfo* getWave = unity::method(unity::klass(game, "Core", "GameplayData"), "get_Wave", 0);
    static const MethodInfo* curType = unity::method(wave, "GetCurrentWaveType", 0);
    static int last = -1;
    seh_assign_wave_kings();
    void* data = gameplay_data();
    if (!data || !getWave) return;
    int year = ((int (*)(void*, const MethodInfo*))il2cpp::method_pointer(getWave))(data, getWave);
    {  // the castle-destroyed handler's view, once per battle
        static bool wasRunning = false;
        bool running = *(bool*)((char*)data + 0x84);  // GameplayData.<Running>
        if (running && !wasRunning) logf("snapshot: battle running, Wave.WaveKing owner %d", wave_king_owner());
        wasRunning = running;
    }
    if (year == last) return;
    last = year;
    void* props = curType ? ((void* (*)(const MethodInfo*))il2cpp::method_pointer(curType))(curType) : nullptr;
    int type = props ? *(int*)((char*)props + 0x14) : -1;
    bool hasKing = props && *(bool*)((char*)props + 0x18);
    int king = hasKing ? *(int*)((char*)props + 0x1C) : 0;
    logf("snapshot: === year %d: wave type %d, attacking king %d", year, type, king);
    log_mp_state();
    Il2CppClass* mm = unity::klass(game, "MultiplayerSystems", "MultiplayerManager");
    char* statics = *(char**)((char*)mm + 0xB8);
    void* session = statics ? *(void**)(statics + il2cpp::field_get_offset(il2cpp::class_get_field_from_name(mm, "Session"))) : nullptr;
    void* runs = session ? *(void**)((char*)session + 0x10) : nullptr;
    size_t n = runs ? *(size_t*)((char*)runs + 0x18) : 0;
    void* losses = session ? *(void**)((char*)session + 0x48) : nullptr;  // GauntletLossPlayerIds
    for (size_t i = 0; i < n; ++i) {
        char* r = ((char**)((char*)runs + 0x20))[i];
        if (r)
            logf("snapshot:     opponent %d: currentHP=%d yearPlayed=%d finalPosition=%d (losses list %d)", *(int*)(r + 0x14),
                 *(int*)(r + 0x2C), *(int*)(r + 0x18), *(int*)(r + 0x1C), losses ? *(int*)((char*)losses + 0x18) : -1);
    }
}

bool battle_started(int& year) {
    const char* game = "Assembly-CSharp.dll";
    static Il2CppClass* wave = unity::klass(game, "Core", "Wave");
    static FieldInfo* dataField = il2cpp::class_get_field_from_name(wave, "Data");
    static Il2CppClass* gd = unity::klass(game, "Core", "GameplayData");
    static const MethodInfo* combatOn = unity::method(gd, "get_CombatOn", 0);
    static const MethodInfo* getWave = unity::method(gd, "get_Wave", 0);
    static bool wasOn = false;
    void* data = nullptr;
    il2cpp::field_static_get_value(dataField, &data);
    if (!data || !combatOn || !getWave) {
        wasOn = false;
        return false;
    }
    bool on = ((bool (*)(void*, const MethodInfo*))il2cpp::method_pointer(combatOn))(data, combatOn);
    bool started = on && !wasOn;
    wasOn = on;
    if (started) year = ((int (*)(void*, const MethodInfo*))il2cpp::method_pointer(getWave))(data, getWave);
    return started;
}

void release(void* snapshot) {
    auto it = g_pinned.find(snapshot);
    if (it == g_pinned.end()) return;
    il2cpp::gchandle_free(it->second);
    g_pinned.erase(it);
}

void self_test() {
    ULONGLONG t0 = GetTickCount64();
    std::string json = capture_json();
    ULONGLONG t1 = GetTickCount64();
    logf("snapshot: self-test captured %zu bytes of JSON in %llu ms", json.size(), t1 - t0);
    if (json.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (mod_dir() + L"\\log\\snapshot_test.json").c_str(), L"wb") == 0 && f) {
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
    }
    void* back = from_json(json);
    ULONGLONG t2 = GetTickCount64();
    if (!back) {
        logf("snapshot: self-test round trip FAILED");
        return;
    }
    // Spot-check: GameSnapshot.Kings (+0x28) and .Areas (+0x30) came back.
    void* kings = *(void**)((char*)back + 0x28);
    void* areas = *(void**)((char*)back + 0x30);
    int areaCount = areas ? *(int*)((char*)areas + 0x18) : -1;
    logf("snapshot: self-test round trip OK in %llu ms: %s, Kings=%p, %d areas", t2 - t1,
         il2cpp::class_get_name(il2cpp::object_get_class(back)), kings, areaCount);
    release(back);
}

// ---------------------------------------------------------------- live opponents
namespace {
// MultiplayerRunData: playerId +0x14, yearPlayed +0x18, kingId +0x28, currentHP +0x2C,
// snapshot (MultiplayerGameSnapshots) +0x50.  MultiplayerGameSnapshots: FinalSnapshot +0x18,
// snapshots (List<GameSnapshot>) +0x28.  GameSnapshot.WaveData +0x20 -> WaveNumber +0x54.
char* find_run(int playerId) {
    void* session = current_session();
    void* runs = session ? *(void**)((char*)session + 0x10) : nullptr;
    size_t n = runs ? *(size_t*)((char*)runs + 0x18) : 0;
    for (size_t i = 0; i < n; ++i) {
        char* r = ((char**)((char*)runs + 0x20))[i];
        if (r && *(int*)(r + 0x14) == playerId) return r;
    }
    return nullptr;
}

int wave_number(void* gameSnapshot) {
    void* wd = gameSnapshot ? *(void**)((char*)gameSnapshot + 0x20) : nullptr;
    return wd ? *(int*)((char*)wd + 0x54) : -1;
}

using AddSnap_t = void (*)(void* self, void* snap, const MethodInfo*);
bool seh_add(AddSnap_t f, void* self, void* snap, const MethodInfo* m) {
    __try { f(self, snap, m); return true; }
    __except (filter(GetExceptionInformation(), "MultiplayerGameSnapshots.Add")) { return false; }
}
} // namespace

std::string year_of(const std::string& json) {
    size_t at = json.find("\"WaveNumber\":");  // the first one is WaveData's
    if (at == std::string::npos) return {};
    at += 13;
    size_t end = at;
    while (end < json.size() && (isdigit((unsigned char)json[end]) || json[end] == '-')) ++end;
    return json.substr(at, end - at);
}

bool put_opponent_snapshot(int playerId, const std::string& json) {
    char* run = find_run(playerId);
    void* snaps = run ? *(void**)(run + 0x50) : nullptr;
    void* list = snaps ? *(void**)((char*)snaps + 0x28) : nullptr;
    if (!list) {
        logf("snapshot: no run record for opponent %d", playerId);
        return false;
    }
    void* obj = from_json(json);
    if (!obj) {
        logf("snapshot: opponent %d: board did not deserialize (%zu bytes)", playerId, json.size());
        return false;
    }
    int year = wave_number(obj);
    // Replace that year's board in place (MultiplayerGameSnapshots.Add ignores a second board
    // for a year it already has), otherwise let the game add it.
    void* items = *(void**)((char*)list + 0x10);
    int count = *(int*)((char*)list + 0x18);
    bool done = false;
    for (int i = 0; items && i < count && !done; ++i) {
        void** slot = (void**)((char*)items + 0x20) + i;
        if (*slot && wave_number(*slot) == year) {
            il2cpp::gc_wbarrier_set_field(items, slot, obj);
            done = true;
        }
    }
    if (!done) {
        static const MethodInfo* add = unity::method(
            unity::klass("Assembly-CSharp.dll", "MultiplayerSystems", "MultiplayerGameSnapshots"), "Add", 1);
        done = add && seh_add((AddSnap_t)il2cpp::method_pointer(add), snaps, obj, add);
    }
    if (done) il2cpp::gc_wbarrier_set_field(snaps, (void**)((char*)snaps + 0x18), obj);  // FinalSnapshot = latest
    logf("snapshot: opponent %d: board for year %d %s (%zu bytes)", playerId, year, done ? "installed" : "NOT installed",
         json.size());
    return done;
}

void set_opponent_hp(int playerId, int hp) {
    char* run = find_run(playerId);
    if (!run) return;
    int& cur = *(int*)(run + 0x2C);
    if (hp > 0) {
        cur = hp;
        *(int*)(run + 0x18) = 99;  // yearPlayed: still in
    } else if (cur > 0 || *(int*)(run + 0x18) == 99) {
        cur = 0;
        int year = current_year();
        *(int*)(run + 0x18) = year < 0 ? 0 : year;  // out after this year
        logf("snapshot: opponent %d is out (year %d)", playerId, year);
    }
}

int opponent_hp(int playerId) {
    char* run = find_run(playerId);
    return run ? *(int*)(run + 0x2C) : -1;
}

bool refresh_opponent_board() {
    // Wave.SpawnWave, not RefreshMultiplayerMap alone: SpawnWave rebuilds the opponent kingdom
    // and then spawns / registers the wave's enemies -- without that second half the opponent's
    // troops stand on their plots but are not enemies, and the player's army ignores them.
    static const MethodInfo* m = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "Wave"), "SpawnWave", 0);
    if (!m) return false;
    return seh_void((void (*)(const MethodInfo*))il2cpp::method_pointer(m), m, "SpawnWave");
}

int my_lives() {
    static FieldInfo* f = il2cpp::class_get_field_from_name(unity::klass("Assembly-CSharp.dll", "Core", "PlayerLife"), "Instance");
    void* pl = nullptr;
    if (f) il2cpp::field_static_get_value(f, &pl);
    return pl ? *(int*)((char*)pl + 0x48) : -1;  // PlayerLife.HealthPoints
}

void log_battle_counts(const char* tag) {
    Il2CppClass* wave = unity::klass("Assembly-CSharp.dll", "Core", "Wave");
    auto i32 = [&](const char* n) {
        int v = -1;
        if (FieldInfo* f = il2cpp::class_get_field_from_name(wave, n)) il2cpp::field_static_get_value(f, &v);
        return v;
    };
    auto coll = [&](const char* n) {  // Wave_TroopCollection.Troops.Count
        void* c = nullptr;
        if (FieldInfo* f = il2cpp::class_get_field_from_name(wave, n)) il2cpp::field_static_get_value(f, &c);
        void* list = c ? *(void**)((char*)c + 0x10) : nullptr;
        return list ? *(int*)((char*)list + 0x18) : -1;
    };
    auto list = [&](const char* n) {
        void* l = nullptr;
        if (FieldInfo* f = il2cpp::class_get_field_from_name(wave, n)) il2cpp::field_static_get_value(f, &l);
        return l ? *(int*)((char*)l + 0x18) : -1;
    };
    logf("snapshot: [%s] allies total %d alive %d ready %d | enemies total %d alive %d ready %d | lists allies %d enemies %d "
         "valid %d | targets: player melee %d ranged %d, enemy melee %d ranged %d notprio %d",
         tag, i32("TotalAllies"), i32("AlliesAlive"), i32("AlliesReady"), i32("TotalEnemies"), i32("EnemiesAlive"),
         i32("EnemiesReady"), list("Allies"), list("Enemies"), list("ValidTroops"), coll("PlayerMeleeTroops"),
         coll("PlayerRangedTroops"), coll("EnemyMeleeTroops"), coll("EnemyRangedTroops"), coll("EnemyNotPriorityTroops"));
}

bool start_battle() {
    static const MethodInfo* m = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "Wave"), "StartWave", 0);
    if (!m) return false;
    return seh_void((void (*)(const MethodInfo*))il2cpp::method_pointer(m), m, "StartWave (debug)");
}

int current_year() {
    static const MethodInfo* getWave = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "GameplayData"), "get_Wave", 0);
    void* data = gameplay_data();
    if (!data || !getWave) return -1;
    return ((int (*)(void*, const MethodInfo*))il2cpp::method_pointer(getWave))(data, getWave);
}

} // namespace nl::snapshot
