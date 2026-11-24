#include "run.h"
#include "log.h"
#include "perks.h"
#include "sync.h"
#include "snapshot.h"

#include <cstdio>
#include "unity.h"

#include <windows.h>
#include <cstring>

namespace nl::run {
namespace {

// Core.Wave.StartNewGame -- as called by KingSelection_KingPreview.StartGame (from disassembly):
//   StartNewGame(king.name, DifficultySO.Instance.GetSelectedDifficulty(), "Gameplay", customEnemies: null,
//                seed: null, insta, disabledKings, isMultiplayer: false, registerRunStart: true,
//                runDefinition: null, isQuest: false)
// Nullable<int> is { bool hasValue; int value; } -> 8 bytes, passed like a uint64.
using StartNewGame_t = void* (*)(void* king, void* difficulty, void* scene, void* customEnemies, uint64_t seed,
                                 bool insta, void* disabledKings, bool isMultiplayer, bool registerRunStart,
                                 void* runDefinition, bool isQuest, const MethodInfo*);
using GetObj_t = void* (*)(const MethodInfo*);
using GetLevel_t = void* (*)(void*, int, const MethodInfo*);
using GetEntry_t = void* (*)(void*, int, const MethodInfo*);
using GetName_t = void* (*)(void*, const MethodInfo*);
using ActiveScene_t = uint64_t (*)(const MethodInfo*);
using SceneName_t = void* (*)(void*, const MethodInfo*);

const MethodInfo *g_startNewGame, *g_diffInstance, *g_diffGetLevel, *g_getEntry, *g_objName, *g_activeScene,
    *g_sceneName;
FieldInfo* g_kingsField = nullptr;
Il2CppClass *g_string = nullptr, *g_ownership = nullptr;

template <class T> T fn(const MethodInfo* m) { return (T)il2cpp::method_pointer(m); }

// Lobby king index -> Core.CardOwnership value, looked up by enum name so a
// reordered enum in a future build still maps correctly.
int ownership_value(int lobbyKing) {
    static const char* names[lobby::kKingCount] = {"KingOfNothing", "KingOfSpells",  "KingOfBlood",
                                                   "KingOfGreed",   "KingOfProgress", "KingOfStone",
                                                   "KingOfNomads",  "KingOfNature",  "KingOfTime"};
    if (lobbyKing < 0 || lobbyKing >= lobby::kKingCount) return -1;
    FieldInfo* f = il2cpp::class_get_field_from_name(g_ownership, names[lobbyKing]);
    if (!f) return -1;
    int v = -1;
    il2cpp::field_static_get_value(f, &v);
    return v;
}

// The KingSo asset name for a CardOwnership value (what StartNewGame takes for kings).
void* king_name_object(int owner) {
    void* kings = nullptr;
    il2cpp::field_static_get_value(g_kingsField, &kings);
    void* entry = kings ? fn<GetEntry_t>(g_getEntry)(kings, owner, g_getEntry) : nullptr;
    void* kingSo = entry ? *(void**)((char*)entry + 0x10) : nullptr;  // KingCollectionEntry.King
    return kingSo ? fn<GetName_t>(g_objName)(kingSo, g_objName) : nullptr;
}

} // namespace

std::string to_utf8(void* s) {
    if (!s) return {};
    int n = il2cpp::string_length(s);
    const wchar_t* w = (const wchar_t*)il2cpp::string_chars(s);
    return narrow(std::wstring(w, n));
}

bool init() {
    const char* game = "Assembly-CSharp.dll";
    const char* core = "UnityEngine.CoreModule.dll";
    g_startNewGame = unity::method(unity::klass(game, "Core", "Wave"), "StartNewGame", 11);
    Il2CppClass* diff = unity::klass(game, "Difficulty", "DifficultySO");
    g_diffInstance = unity::method(diff, "get_Instance", 0);
    g_diffGetLevel = unity::method(diff, "GetLevel", {"System.Int32"});
    g_getEntry = unity::method(unity::klass(game, "Core.Entities", "KingCollectionSo"), "GetEntry", 1);
    g_kingsField = il2cpp::class_get_field_from_name(unity::klass(game, "KingManagement", "KingManager"), "kings");
    g_ownership = unity::klass(game, "Core", "CardOwnership");
    g_objName = unity::method(unity::klass(core, "UnityEngine", "Object"), "get_name", 0);
    g_string = unity::klass("mscorlib.dll", "System", "String");
    g_activeScene = unity::method(unity::klass(core, "UnityEngine.SceneManagement", "SceneManager"), "GetActiveScene", 0);
    g_sceneName = unity::method(unity::klass(core, "UnityEngine.SceneManagement", "Scene"), "get_name", 0);

    bool ok = g_startNewGame && g_diffInstance && g_diffGetLevel && g_getEntry && g_kingsField && g_ownership &&
              g_objName && g_string;
    logf("run: %s", ok ? "ready" : "missing game methods -- cannot start runs");
    for (int k = 0; k < lobby::kKingCount; ++k) logf("run: lobby king %d -> CardOwnership %d", k, ownership_value(k));
    return ok;
}

namespace {

bool g_mpRun = false;  // a NineLink multiplayer-mode run is in progress
std::string read_file(const std::wstring& path);  // defined below
Il2CppClass* g_slotSettings = nullptr;
const MethodInfo *g_slotCtor = nullptr, *g_activePerks = nullptr, *g_getLoadout = nullptr, *g_kmSave = nullptr,
                 *g_tryGetKs = nullptr;
FieldInfo *g_perksUsed = nullptr, *g_kingSettingsField = nullptr, *g_psX = nullptr, *g_psY = nullptr,
          *g_psTerm = nullptr, *g_psLevel = nullptr;
bool g_plumbing = false;

// The swapped-in loadout, restored once the run has applied its perks.
struct Swap {
    void* list = nullptr;       // the game's own PerksUsed List object (kept; only its contents change)
    void* origItems = nullptr;  // its backing array before we swapped
    int origSize = 0;
    void *hList = nullptr, *hItems = nullptr;
    ULONGLONG since = 0;
    int owner = 0;
    bool active = false;
} g_swap;

void* keep(void* obj);  // pin a fresh managed object (defined below)

// Generic List<T> methods are shared code: their inflated MethodInfo may carry no direct
// method pointer, so they go through il2cpp_runtime_invoke, which handles the sharing.
bool invoke(const MethodInfo* m, void* obj, void** args, const char* what) {
    if (!m) {
        logf("run: %s: method not found", what);
        return false;
    }
    void* exc = nullptr;
    il2cpp::runtime_invoke(m, obj, args, &exc);
    if (exc) {
        il2cpp::log_exception(exc, what);
        return false;
    }
    return true;
}

// Lists we allocate ourselves crash the runtime once the game serialises them, whatever they
// hold. So the game's own List object is reused: only its backing array (_items) and _size
// are swapped, and put back afterwards.
void set_list_contents(void* list, void* arr, int size) {
    Il2CppClass* k = il2cpp::object_get_class(list);
    il2cpp::field_set_value(list, il2cpp::class_get_field_from_name(k, "_items"), &arr);
    il2cpp::field_set_value(list, il2cpp::class_get_field_from_name(k, "_size"), &size);
}

void* typed_array(const std::vector<void*>& items) {
    void* arr = keep(il2cpp::array_new(g_slotSettings, items.size()));
    void** elems = (void**)((char*)arr + 0x20);
    for (size_t i = 0; i < items.size(); ++i) il2cpp::gc_wbarrier_set_field(arr, &elems[i], items[i]);
    return arr;
}

// Every managed object we allocate is pinned with a GC handle straight away: between creation
// and being stored into a managed field it is referenced only from native memory (e.g. a
// std::vector), which the GC does not scan -- a collection in that window frees it and the
// game later trips over the dangling reference. Released when the swap is undone.
std::vector<void*> g_keepAlive;
void* keep(void* obj) {
    if (obj) g_keepAlive.push_back(il2cpp::gchandle_new(obj, false));
    return obj;
}
void release_kept() {
    for (void* h : g_keepAlive) il2cpp::gchandle_free(h);
    g_keepAlive.clear();
}

void* new_slot_settings(int x, int y, const std::string& term, int level) {
    void* s = keep(il2cpp::object_new(g_slotSettings));
    // Non-generic constructor: called directly, not through il2cpp_runtime_invoke.
    ((void (*)(void*, const MethodInfo*))il2cpp::method_pointer(g_slotCtor))(s, g_slotCtor);
    il2cpp::field_set_value(s, g_psX, &x);
    il2cpp::field_set_value(s, g_psY, &y);
    void* str = term.empty() ? nullptr : keep(unity::str(term.c_str()));
    il2cpp::field_set_value(s, g_psTerm, &str);  // il2cpp dereferences: pass a pointer to the reference
    il2cpp::field_set_value(s, g_psLevel, &level);
    return s;
}

void resolve_plumbing() {
    if (g_plumbing) return;
    g_plumbing = true;
    const char* game = "Assembly-CSharp.dll";
    g_slotSettings = unity::klass(game, "PerkSystem", "PerkSlotSettings");
    g_slotCtor = unity::method(g_slotSettings, ".ctor", 0);
    g_psX = il2cpp::class_get_field_from_name(g_slotSettings, "x");
    g_psY = il2cpp::class_get_field_from_name(g_slotSettings, "y");
    g_psTerm = il2cpp::class_get_field_from_name(g_slotSettings, "perkTerm");
    g_psLevel = il2cpp::class_get_field_from_name(g_slotSettings, "level");
    Il2CppClass* km = unity::klass(game, "KingManagement", "KingManager");
    g_activePerks = unity::method(km, "GetAllKingActivePerks", 1);
    g_kmSave = unity::method(km, "Save", 0);
    g_kingSettingsField = il2cpp::class_get_field_from_name(km, "kingSettings");
    g_getLoadout = unity::method(unity::klass(game, "KingManagement", "KingSettings"), "GetActivePerkLoadout", 1);
    g_perksUsed = il2cpp::class_get_field_from_name(unity::klass(game, "KingManagement", "PerkLoadoutSettings"), "PerksUsed");
}

int list_count(void* list) { return list ? *(int*)((char*)list + 0x18) : -1; }  // List<T>._size

// The PerkLoadoutSettings GetAllKingActivePerks reads during a single-player run:
// KingManager.kingSettings[owner].GetActivePerkLoadout(multiplayer: false).
// KingManager.kingSettings[owner], read straight from Dictionary's storage. (Calling
// TryGetValue through il2cpp_runtime_invoke left the runtime in a state where the game's
// next serialisation crashed, so this path makes no runtime_invoke calls at all.)
void* king_settings(int owner) {
    void* dict = nullptr;
    il2cpp::field_static_get_value(g_kingSettingsField, &dict);
    if (!dict) return nullptr;
    size_t entriesOff = 0, countOff = 0;
    for (Il2CppClass* k = il2cpp::object_get_class(dict); k && (!entriesOff || !countOff); k = il2cpp::class_get_parent(k)) {
        void* it = nullptr;
        while (FieldInfo* f = il2cpp::class_get_fields(k, &it)) {
            if (il2cpp::field_get_flags(f) & 0x10) continue;  // static
            std::string n = il2cpp::field_get_name(f);
            if (n == "_entries" || n == "entries") entriesOff = il2cpp::field_get_offset(f);
            if (n == "_count" || n == "count") countOff = il2cpp::field_get_offset(f);
        }
    }
    if (!entriesOff || !countOff) {
        logf("run: Dictionary layout not recognised");
        return nullptr;
    }
    char* entries = *(char**)((char*)dict + entriesOff);
    int count = *(int*)((char*)dict + countOff);
    // Entry<CardOwnership, KingSettings> = { int hashCode; int next; int key; (pad); KingSettings value } = 24 bytes
    for (int i = 0; entries && i < count; ++i) {
        char* e = entries + 0x20 + (size_t)i * 24;
        int next = *(int*)(e + 4), key = *(int*)(e + 8);
        void* value = *(void**)(e + 16);
        if (next >= -1 && key == owner && value) {
            const char* cls = il2cpp::class_get_name(il2cpp::object_get_class(value));
            if (strcmp(cls, "KingSettings") == 0) return value;
            logf("run: Dictionary entry for %d holds a %s, not KingSettings", owner, cls);
            return nullptr;
        }
    }
    logf("run: no KingSettings for CardOwnership %d (%d entries)", owner, count);
    return nullptr;
}

// The PerkLoadoutSettings GetAllKingActivePerks reads during a single-player run:
// KingManager.kingSettings[owner].GetActivePerkLoadout(multiplayer: false).
void* active_loadout(int owner) {
    void* ks = king_settings(owner);
    if (!ks) return nullptr;
    return ((void* (*)(void*, bool, const MethodInfo*))il2cpp::method_pointer(g_getLoadout))(ks, false, g_getLoadout);
}

void restore_loadout(const char* why) {
    if (!g_swap.active) return;
    set_list_contents(g_swap.list, g_swap.origItems, g_swap.origSize);
    il2cpp::gchandle_free(g_swap.hList);
    if (g_swap.hItems) il2cpp::gchandle_free(g_swap.hItems);
    release_kept();
    g_swap = {};
    // Write the player's own loadout back in case the game saved while ours was in place.
    if (g_kmSave) ((void (*)(const MethodInfo*))il2cpp::method_pointer(g_kmSave))(g_kmSave);
    logf("run: king's own perk loadout restored (%s)", why);
}

// Puts our grid in place of the king's active loadout until the run has applied it.
bool swap_in_loadout(int owner, const std::vector<loadout::Entry>& perks) {
    resolve_plumbing();
    if (!g_perksUsed || !g_getLoadout || !g_kingSettingsField) return false;
    restore_loadout("superseded");
    void* lo = active_loadout(owner);
    if (!lo) {
        logf("run: no active perk loadout for CardOwnership %d", owner);
        return false;
    }
    void* original = *(void**)((char*)lo + il2cpp::field_get_offset(g_perksUsed));
    if (!original) {
        logf("run: the king's loadout has no PerksUsed list -- perks not applied");
        return false;
    }
    std::vector<void*> items;
    bool bisectEmpty = GetFileAttributesW((mod_dir() + L"\\debug_empty.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    for (const loadout::Entry& e : perks) {
        if (bisectEmpty) break;
        items.push_back(new_slot_settings(e.x, e.y, e.term, e.level));
        logf("run: perk (%d,%d) %s x%d", e.x, e.y, e.term.c_str(), e.level);
    }
    if (bisectEmpty) logf("run: debug: swapping in an EMPTY array");
    g_swap.list = original;
    g_swap.origItems = *(void**)((char*)original + 0x10);  // List._items
    g_swap.origSize = list_count(original);
    g_swap.hList = il2cpp::gchandle_new(original, false);
    g_swap.hItems = g_swap.origItems ? il2cpp::gchandle_new(g_swap.origItems, false) : 0;
    set_list_contents(original, typed_array(items), (int)items.size());
    g_swap.active = true;
    g_swap.since = 0;
    g_swap.owner = owner;

    // (GetAllKingActivePerks cannot be called here to double-check: it needs Wave.Data, which
    // only exists once the run has started.)
    logf("run: swapped in %zu perks (king's own list had %d)", perks.size(), g_swap.origSize);
    return true;
}

} // namespace

int seh_filter(EXCEPTION_POINTERS* ep, const char* what) {
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t ga = il2cpp::base;
    logf("run: %s: native exception 0x%08lX at %p (GameAssembly+0x%llX)", what, ep->ExceptionRecord->ExceptionCode,
         (void*)addr, (unsigned long long)(addr - ga));
    // Unwind the faulting stack with the x64 unwind tables and log each frame's GameAssembly RVA.
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 16 && ctx.Rip; ++i) {
        logf("run:   frame %2d  GameAssembly+0x%llX", i, (unsigned long long)(ctx.Rip - ga));
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!f) {
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, f, &ctx, &handlerData, &establisher, nullptr);
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;  // C++ throw: let catch see it
    return EXCEPTION_EXECUTE_HANDLER;
}

bool call_start_new_game(StartNewGame_t f, void* king, void* diff, void* scene, void* customEnemies, uint64_t seed,
                         void* disabled, void* runDef, bool multiplayer, const MethodInfo* m) {
    __try {
        f(king, diff, scene, customEnemies, seed, false, disabled, multiplayer, true, runDef, false, m);
        return true;
    } __except (seh_filter(GetExceptionInformation(), "StartNewGame")) {
        return false;
    }
}

bool call_void_seh(void (*f)(const MethodInfo*), const MethodInfo* m, const char* what) {
    __try {
        f(m);
        return true;
    } __except (seh_filter(GetExceptionInformation(), what)) {
        return false;
    }
}

bool start(const lobby::StartInfo& info, const std::vector<loadout::Entry>& perkList,
           const std::vector<std::pair<int, int>>& unlockedSlots) {
    if (!g_startNewGame) return false;
    int owner = ownership_value(info.myKing);
    if (owner < 0) {
        logf("run: no king chosen (%d)", info.myKing);
        return false;
    }

    void* kings = nullptr;
    il2cpp::field_static_get_value(g_kingsField, &kings);
    void* entry = kings ? fn<GetEntry_t>(g_getEntry)(kings, owner, g_getEntry) : nullptr;
    void* kingSo = entry ? *(void**)((char*)entry + 0x10) : nullptr;  // KingCollectionEntry.King
    void* kingName = kingSo ? fn<GetName_t>(g_objName)(kingSo, g_objName) : nullptr;
    if (!kingName) {
        logf("run: could not resolve the KingSo for CardOwnership %d", owner);
        return false;
    }

    // PvP has no difficulty setting: everyone plays on the base level.
    void* diffSo = fn<GetObj_t>(g_diffInstance)(g_diffInstance);
    void* difficulty = diffSo ? fn<GetLevel_t>(g_diffGetLevel)(diffSo, 0, g_diffGetLevel) : nullptr;

    void* noDisabledKings = il2cpp::array_new(g_string, 0);
    uint64_t seed = 1ull | ((uint64_t)(uint32_t)info.seed << 32);  // Nullable<int>{ hasValue = true, value = seed }

    logf("run: StartNewGame king=%s (owner %d) difficulty=%p seed=%d players=%zu", to_utf8(kingName).c_str(), owner,
         difficulty, info.seed, info.players.size());
    // KingSelection_KingPreview.StartGame picks one of two string literals for the scene
    // (the second only with Banisher perk stacks); the normal one is the "Gameplay" scene.
    // StartNewGame's LoadScene coroutine hands it straight to Core.Scene.Begin, so it must not be null.
    void* scene = unity::str("Gameplay");
    // Perks: RunDefinition.Perks is not usable (the game snapshots the RunDefinition through
    // Newtonsoft, and serialising a List<PerkSlotSettings> crashes this IL2CPP build). Instead the
    // king's active loadout is swapped for ours until GameplayManager.ApplyPerks has read it.
    // DISABLED by default: swapping the king's loadout crashed the run, and a crash before the
    // swap is undone lets the game save our list over the player's own loadout. Until a safe
    // hand-off exists, perks are configured in the lobby but not applied (debug_perkswap.txt
    // re-enables the experiment).
    if (!perkList.empty() && GetFileAttributesW((mod_dir() + L"\\debug_perkswap.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
        swap_in_loadout(owner, perkList);
    else if (!perkList.empty())
        logf("run: %zu perks chosen -- not applied yet (perk hand-off disabled)", perkList.size());
    // Experiment: debug_mpself.txt starts in multiplayer mode against our own exported snapshot
    // (log\snapshot_test.json) installed as the only opponent.
    bool multiplayer = false;
    void* customEnemies = nullptr;
    if (GetFileAttributesW((mod_dir() + L"\\debug_mpself.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
        // The opponent plays King of Greed (lobby king 3); the run's only enemy king is Greed, so
        // every year's attacker maps to an opponent (Session.GetDataByPlayerKing).
        int oppLobbyKing = 3, oppOwner = ownership_value(oppLobbyKing);
        void* oppName = king_name_object(oppOwner);
        if (oppName) {
            customEnemies = il2cpp::array_new(g_string, 1);
            il2cpp::gc_wbarrier_set_field(customEnemies, (void**)((char*)customEnemies + 0x20), oppName);
            il2cpp::gchandle_new(customEnemies, false);
        }
        logf("run: experiment: opponent king %s (owner %d)", oppName ? to_utf8(oppName).c_str() : "?", oppOwner);
        std::string snap;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (mod_dir() + L"\\log\\snapshot_test.json").c_str(), L"rb") == 0 && f) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) snap.append(buf, n);
            fclose(f);
        }
        // One snapshot per year 0..33. Recorded boards (log\rec_year_<y>.json, see debug_record)
        // are used where present; a year without one reuses the latest earlier board with its
        // WaveData.WaveNumber rewritten -- the same "placeholder" a live opponent will get.
        std::vector<std::string> perYear;
        std::string latest = snap;
        int recorded = 0;
        for (int y = 0; y <= 33; ++y) {
            wchar_t name[64];
            swprintf_s(name, L"\\log\\rec_year_%d.json", y);
            std::string rec = read_file(mod_dir() + name);
            if (!rec.empty()) {
                latest = rec;
                perYear.push_back(rec);
                ++recorded;
                continue;
            }
            size_t at = latest.find("\"WaveNumber\":");
            size_t end = at == std::string::npos ? at : latest.find(',', at);
            if (at == std::string::npos || end == std::string::npos) break;
            perYear.push_back(latest.substr(0, at) + "\"WaveNumber\":" + std::to_string(y) + latest.substr(end));
        }
        logf("run: experiment: %zu yearly snapshots for the test rival (%d recorded)", perYear.size(), recorded);
        if (!perYear.empty() && snapshot::install_session({{2, "Test Rival", oppOwner, perYear}})) multiplayer = true;
        logf("run: experiment: multiplayer mode %s", multiplayer ? "ON" : "could not be set up");
    }
    // A lobby game: every other player is an opponent (multiplayer mode), playing their own king.
    // Their boards arrive year by year (sync); until the first one, a bare castle stands in.
    std::vector<sync::Peer> peers;
    if (!multiplayer && info.players.size() >= 2) {
        std::string tmpl = read_file(mod_dir() + L"\\empty_kingdom.json");
        std::vector<snapshot::Opponent> opps;
        std::vector<void*> enemyNames;
        int pid = 2;
        for (const lobby::Member& m : info.players) {
            if (m.me) continue;
            int o = ownership_value(m.king);
            void* n = o > 0 ? king_name_object(o) : nullptr;
            if (!n || m.king == info.myKing) {
                logf("run: player %s has no usable king (%d) -- skipped", m.name.c_str(), m.king);
                continue;
            }
            // The game's multiplayer screens use a Latin-only font: keep ASCII names readable.
            std::string ascii;
            for (char c : m.name)
                if (c >= 0x20 && c < 0x7F) ascii += c;
            if (ascii.empty()) ascii = "Player " + std::to_string(pid - 1);
            opps.push_back({pid, ascii, o, {tmpl}});
            peers.push_back({m.id, pid, m.name, m.king});
            enemyNames.push_back(n);
            ++pid;
        }
        if (tmpl.empty()) logf("run: empty_kingdom.json missing beside the DLL");
        if (!opps.empty() && !tmpl.empty()) {
            customEnemies = il2cpp::array_new(g_string, enemyNames.size());
            for (size_t i = 0; i < enemyNames.size(); ++i)
                il2cpp::gc_wbarrier_set_field(customEnemies, (void**)((char*)customEnemies + 0x20) + i, enemyNames[i]);
            il2cpp::gchandle_new(customEnemies, false);
            multiplayer = snapshot::install_session(opps);
        }
        logf("run: lobby game with %zu opponent(s): multiplayer mode %s", opps.size(), multiplayer ? "ON" : "FAILED");
        if (!multiplayer) peers.clear();
    }
    if (!call_start_new_game(fn<StartNewGame_t>(g_startNewGame), kingName, difficulty, scene, customEnemies, seed,
                             noDisabledKings, nullptr, multiplayer, g_startNewGame)) {
        restore_loadout("start failed");
        return false;
    }
    logf("run: StartNewGame returned");
    if (multiplayer) {
        g_mpRun = true;
        snapshot::block_official_submit(true);
        snapshot::prepare_gauntlet();
        snapshot::assign_wave_kings();
        if (!peers.empty()) sync::begin(peers);
    }
    return true;
}

void diag() {
    Il2CppClass* wave = unity::klass("Assembly-CSharp.dll", "Core", "Wave");
    void *playerKing = nullptr, *data = nullptr;
    il2cpp::field_static_get_value(il2cpp::class_get_field_from_name(wave, "PlayerKing"), &playerKing);
    il2cpp::field_static_get_value(il2cpp::class_get_field_from_name(wave, "Data"), &data);
    int seed = -1;
    const MethodInfo* getSeed = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "GameplayData"), "get_Seed", 0);
    if (data && getSeed) seed = ((int (*)(void*, const MethodInfo*))il2cpp::method_pointer(getSeed))(data, getSeed);
    logf("run: after start PlayerKing=%p GameplayData=%p seed=%d", playerKing, data, seed);
}

const std::vector<std::string>& own_perks(int lobbyKing) {
    static std::vector<std::string> names;
    static int cachedKing = -2;
    static ULONGLONG cachedAt = 0;
    ULONGLONG now = GetTickCount64();
    if (lobbyKing == cachedKing && now - cachedAt < 2000) return names;
    cachedKing = lobbyKing;
    cachedAt = now;
    names.clear();
    resolve_plumbing();
    int owner = ownership_value(lobbyKing);
    void* lo = owner >= 0 && g_getLoadout && g_perksUsed ? active_loadout(owner) : nullptr;
    void* list = lo ? *(void**)((char*)lo + il2cpp::field_get_offset(g_perksUsed)) : nullptr;
    int n = list_count(list);
    void* items = list ? *(void**)((char*)list + 0x10) : nullptr;
    for (int i = 0; items && i < n; ++i) {
        void* s = ((void**)((char*)items + 0x20))[i];
        if (!s) continue;
        std::string term = to_utf8(*(void**)((char*)s + 0x18));  // PerkSlotSettings.perkTerm
        int level = *(int*)((char*)s + 0x20);                     // PerkSlotSettings.level
        if (term.empty()) continue;
        perks::Perk* p = perks::by_term(term);
        std::string name = p ? p->name : term;
        if (level > 1) name += " x" + std::to_string(level);
        names.push_back(name);
    }
    return names;
}

bool mp_active() { return g_mpRun; }

namespace {
std::string read_file(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}
} // namespace

void tick(const std::string& scene) {
    // Developer tool: debug_record.txt saves the board at the start of every battle
    // (preparation done) as log\rec_year_<year>.json -- real yearly snapshots for testing.
    if (scene == "Gameplay") {
        int year = -1;
        static ULONGLONG countAt[2] = {};
        static bool debugCounts = GetFileAttributesW((mod_dir() + L"\\debug_counts.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
        ULONGLONG now = GetTickCount64();
        for (ULONGLONG& t : countAt)
            if (t && now >= t) {
                t = 0;
                snapshot::log_battle_counts("in battle");
            }
        // Developer tool: debug_autobattle.txt starts every battle by itself 8 s into the year.
        static int autoYear = -1;
        static ULONGLONG yearSince = 0;
        static bool autoBattle = GetFileAttributesW((mod_dir() + L"\\debug_autobattle.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (autoBattle) {
            int y = snapshot::current_year();
            if (y != autoYear) {
                autoYear = y;
                yearSince = now;
            } else if (yearSince && now - yearSince > 8000) {
                yearSince = 0;
                logf("run: debug_autobattle -- starting the battle of year %d", y);
                snapshot::start_battle();
            }
        }
        if (snapshot::battle_started(year)) {
            logf("run: battle of year %d begins (preparation finished)", year);
            if (debugCounts) {
                snapshot::log_battle_counts("battle start");
                countAt[0] = now + 3000;
                countAt[1] = now + 8000;
            }
            if (GetFileAttributesW((mod_dir() + L"\\debug_record.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::string json = snapshot::capture_json();
                FILE* f = nullptr;
                wchar_t name[64];
                swprintf_s(name, L"\\log\\rec_year_%d.json", year);
                if (!json.empty() && _wfopen_s(&f, (mod_dir() + name).c_str(), L"wb") == 0 && f) {
                    fwrite(json.data(), 1, json.size(), f);
                    fclose(f);
                    logf("run: recorded year %d (%zu bytes)", year, json.size());
                }
            }
        }
    }
    if (g_mpRun) {
        static bool seenGameplay = false;
        if (scene == "Gameplay") {
            seenGameplay = true;
            snapshot::watch_years();
        } else if ((scene == "MainMenu" || scene == "MultiplayerHub") && seenGameplay) {  // the run is over
            seenGameplay = false;
            g_mpRun = false;
            snapshot::block_official_submit(false);
            sync::end();
            lobby::end_game();
        }
    }
    if (!g_swap.active) return;
    // GameplayManager.Start applies the perks right after the Gameplay scene loads; give it a moment.
    if (scene == "Gameplay") {
        if (!g_swap.since) g_swap.since = GetTickCount64();
        else if (GetTickCount64() - g_swap.since > 8000) {
            // In the run now: confirm what the game reads before giving the loadout back.
            void* seen = ((void* (*)(int, const MethodInfo*))il2cpp::method_pointer(g_activePerks))(g_swap.owner, g_activePerks);
            logf("run: in the run, GetAllKingActivePerks returns %d perks", list_count(seen));
            restore_loadout("perks applied");
        }
    } else if (scene == "MainMenu" && g_swap.since) {
        restore_loadout("back on the main menu");
    }
}

std::string active_scene() {
    if (!g_activeScene || !g_sceneName) return {};
    uint64_t scene = fn<ActiveScene_t>(g_activeScene)(g_activeScene);  // Scene is a small struct (handle)
    return to_utf8(fn<SceneName_t>(g_sceneName)(&scene, g_sceneName));
}

} // namespace nl::run
