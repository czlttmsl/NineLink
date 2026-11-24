#include "sync.h"
#include "lobby.h"
#include "log.h"
#include "patch.h"
#include "snapshot.h"
#include "unity.h"

#include <windows.h>
#include <cstring>
#include <map>
#include <set>

namespace nl::sync {
namespace {

// Messages (lobby data channel, fields separated by '|', the board JSON last):
//   B|<year>|<lives>|<json>   my board for <year>, captured when I started that battle
//   A|<year>|<toId>           ack: I have <toId>'s board for <year> (stops the resends)
//   D                         I am out (no lives left / left the run)
struct PeerState {
    Peer p;
    std::map<int, std::string> boards;  // year -> board JSON
    int lives = -1;                     // as last reported by them
    bool out = false;
    std::set<int> acked;                // years of my board they confirmed
    ULONGLONG missingSince = 0;         // not in the room member list since (0 = present)
};

bool g_active = false;
std::vector<PeerState> g_peers;
std::map<int, std::string> g_myBoards;  // year -> my board JSON (kept for resends)
int g_waitYear = -1;                    // holding this year's battle back
ULONGLONG g_waitSince = 0, g_releaseAt = 0, g_lastResend = 0;
bool g_boardsInstalled = false;
bool g_forceRelease = false;  // stop_waiting(): go ahead without the missing boards
int g_releaseYear = -1;  // StartWave passes straight through for this year
bool g_sentOut = false;

patch::Redirect g_hook;
void* g_startWave = nullptr;  // Wave.StartWave entry
const MethodInfo* g_startWaveMi = nullptr;

bool seh_call_fwd(void (*f)(const MethodInfo*), const MethodInfo* m) {
    __try { f(m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Wave.SpawnWave rebuilds the opponent kingdom from the run records (RefreshMultiplayerMap) and
// registers the wave's enemies. The game calls it at the start of some years only; in a
// synchronised run it runs exactly once per year, when every friend's board is in (the board the
// battle must use). The game's own calls are skipped then: a second rebuild in the same year would
// spawn the opponent's troops twice.
patch::Redirect g_spawnHook;
void* g_spawnWave = nullptr;
const MethodInfo* g_spawnWaveMi = nullptr;
bool g_allowRefresh = false;  // our own call at the barrier
bool g_seenFirstSpawn = false;  // the game's first SpawnWave of this run went through

void HookSpawnWave(const MethodInfo* m);

void call_spawn_wave() {
    g_spawnHook.remove();
    bool ok = seh_call_fwd((void (*)(const MethodInfo*))g_spawnWave, g_spawnWaveMi);
    g_spawnHook.install(g_spawnWave, (void*)&HookSpawnWave);
    if (!ok) logf("sync: Wave.SpawnWave faulted");
}

void HookSpawnWave(const MethodInfo*) {
    // The run's first SpawnWave (start of year 1) sets the opponent castle up -- skipping it leaves
    // PlayerBase without its setup during the first preparation. It only shows the bare starting
    // castle, so building it twice that year is harmless.
    if (g_active && !g_allowRefresh && g_seenFirstSpawn) {
        logf("sync: year %d: the game's SpawnWave skipped (it runs once all boards are in)", snapshot::current_year());
        return;
    }
    if (g_active) g_seenFirstSpawn = true;
    call_spawn_wave();
}

PeerState* find_peer(uint64_t id) {
    for (PeerState& s : g_peers)
        if (s.p.id == id) return &s;
    return nullptr;
}

bool seh_call(void (*f)(const MethodInfo*), const MethodInfo* m) {
    __try { f(m); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HookStartWave(const MethodInfo* m);

// The game's own StartWave: lift the redirect for the call, then put it back.
void call_start_wave() {
    g_hook.remove();
    bool ok = seh_call((void (*)(const MethodInfo*))g_startWave, g_startWaveMi);
    g_hook.install(g_startWave, (void*)&HookStartWave);
    if (!ok) logf("sync: Wave.StartWave faulted");
}

void send_board(uint64_t to, int year) {
    auto it = g_myBoards.find(year);
    if (it == g_myBoards.end()) return;
    lobby::send_data(to, "B|" + std::to_string(year) + "|" + std::to_string(snapshot::my_lives()) + "|" + it->second);
}

void HookStartWave(const MethodInfo*) {
    int year = snapshot::current_year();
    if (!g_active || year < 0 || year == g_releaseYear) {
        call_start_wave();
        return;
    }
    if (g_waitYear == year) return;  // already waiting for this year's battle
    std::string json = snapshot::capture_json();
    if (json.empty()) {  // cannot sync this year: do not hold the player hostage
        logf("sync: year %d: could not capture the board -- battle starts unsynchronised", year);
        call_start_wave();
        return;
    }
    g_myBoards[year] = std::move(json);
    g_waitYear = year;
    g_waitSince = GetTickCount64();
    g_releaseAt = 0;
    g_boardsInstalled = false;
    g_lastResend = g_waitSince;
    logf("sync: year %d: preparation done, board captured (%zu bytes) -- waiting for friends", year,
         g_myBoards[year].size());
    send_board(0, year);
}

void mark_out(PeerState& s, const char* why) {
    if (s.out) return;
    s.out = true;
    snapshot::set_opponent_hp(s.p.playerId, 0);
    logf("sync: %s is out (%s)", s.p.name.c_str(), why);
}

void handle(uint64_t from, const std::string& msg) {
    PeerState* s = find_peer(from);
    if (!s || msg.size() < 1) return;
    if (msg[0] == 'B') {
        size_t a = msg.find('|', 2), b = a == std::string::npos ? a : msg.find('|', a + 1);
        if (b == std::string::npos) return;
        int year = atoi(msg.substr(2, a - 2).c_str());
        int lives = atoi(msg.substr(a + 1, b - a - 1).c_str());
        if (!s->boards.count(year)) {
            s->boards[year] = msg.substr(b + 1);
            logf("sync: board of %s for year %d arrived (%zu bytes, %d lives)", s->p.name.c_str(), year,
                 s->boards[year].size(), lives);
        }
        s->lives = lives;
        lobby::send_data(from, "A|" + std::to_string(year) + "|" + std::to_string(lobby::my_id()));
    } else if (msg[0] == 'A') {
        size_t a = msg.find('|', 2);
        if (a == std::string::npos) return;
        uint64_t to = _strtoui64(msg.substr(a + 1).c_str(), nullptr, 10);
        if (to == lobby::my_id()) s->acked.insert(atoi(msg.substr(2, a - 2).c_str()));
    } else if (msg[0] == 'D') {
        mark_out(*s, "no lives left");
    }
}

// Friends who left the room are out (after a grace period: member lists refresh with a delay).
void check_presence(ULONGLONG now) {
    const auto& members = lobby::members();
    for (PeerState& s : g_peers) {
        if (s.out) continue;
        bool here = false;
        for (const auto& m : members) here |= m.id == s.p.id;
        if (here) s.missingSince = 0;
        else if (!s.missingSince) s.missingSince = now;
        else if (now - s.missingSince > 15000) mark_out(s, "left the room");
    }
}

void advance_barrier(ULONGLONG now) {
    if (g_waitYear < 0) return;
    // Resend my board to whoever has not confirmed it yet (covers Steam session setup).
    if (now - g_lastResend > 3000) {
        g_lastResend = now;
        for (PeerState& s : g_peers)
            if (!s.out && !s.acked.count(g_waitYear)) send_board(s.p.id, g_waitYear);
    }
    if (!g_boardsInstalled) {
        if (!g_forceRelease)
            for (const PeerState& s : g_peers)
                if (!s.out && !s.boards.count(g_waitYear)) return;  // still waiting
        g_forceRelease = false;
        // Everyone is here: their boards go into their run records, then the kingdom is rebuilt.
        for (PeerState& s : g_peers) {
            if (s.out) continue;
            auto it = s.boards.find(g_waitYear);
            if (it != s.boards.end()) snapshot::put_opponent_snapshot(s.p.playerId, it->second);
            if (s.lives > 0) snapshot::set_opponent_hp(s.p.playerId, s.lives);
            else if (s.lives == 0) mark_out(s, "reported 0 lives");
        }
        g_allowRefresh = true;
        snapshot::refresh_opponent_board();
        g_allowRefresh = false;
        g_boardsInstalled = true;
        g_releaseAt = now + 1500;  // RefreshMultiplayerMap awaits the areas: give it a moment
        logf("sync: year %d: all boards in after %.1f s -- battle starts shortly", g_waitYear, (now - g_waitSince) / 1000.0);
        return;
    }
    if (now >= g_releaseAt) {
        g_releaseYear = g_waitYear;
        g_waitYear = -1;
        logf("sync: year %d: battle released", g_releaseYear);
        snapshot::log_battle_counts("before StartWave");
        call_start_wave();
    }
}

} // namespace

bool install() {
    g_startWaveMi = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "Wave"), "StartWave", 0);
    g_startWave = g_startWaveMi ? il2cpp::method_pointer(g_startWaveMi) : nullptr;
    bool ok = g_startWave && g_hook.install(g_startWave, (void*)&HookStartWave);
    logf("sync: Wave.StartWave %s", ok ? "hooked" : "NOT hooked -- battles will not be synchronised");
    g_spawnWaveMi = unity::method(unity::klass("Assembly-CSharp.dll", "Core", "Wave"), "SpawnWave", 0);
    g_spawnWave = g_spawnWaveMi ? il2cpp::method_pointer(g_spawnWaveMi) : nullptr;
    bool spawnOk = g_spawnWave && g_spawnHook.install(g_spawnWave, (void*)&HookSpawnWave);
    logf("sync: Wave.SpawnWave %s", spawnOk ? "hooked" : "NOT hooked -- opponent troops may spawn twice");
    return ok;
}

void begin(const std::vector<Peer>& peers) {
    g_peers.clear();
    for (const Peer& p : peers) g_peers.push_back({p});
    g_myBoards.clear();
    g_waitYear = g_releaseYear = -1;
    g_sentOut = false;
    g_seenFirstSpawn = false;
    g_forceRelease = false;
    g_active = !g_peers.empty();
    // Drop anything left over from the lobby phase.
    uint64_t from;
    std::string msg;
    while (lobby::recv_data(from, msg)) {}
    logf("sync: synchronised PvP with %zu friend(s), my id %llu", g_peers.size(), (unsigned long long)lobby::my_id());
}

void end() {
    if (!g_active) return;
    if (!g_sentOut) lobby::send_data(0, "D");
    g_active = false;
    g_waitYear = -1;
    g_peers.clear();
    g_myBoards.clear();
    logf("sync: run over");
}

bool active() { return g_active; }

void tick() {
    if (!g_active) return;
    ULONGLONG now = GetTickCount64();
    uint64_t from;
    std::string msg;
    while (lobby::recv_data(from, msg)) handle(from, msg);
    check_presence(now);
    if (!g_sentOut && snapshot::my_lives() == 0) {
        g_sentOut = true;
        lobby::send_data(0, "D");
        logf("sync: I am out of lives");
    }
    // A friend still in the game never drops to 0 lives here: my own battle against them only
    // decides it locally, their game is the authority (their next report corrects it).
    for (PeerState& s : g_peers)
        if (!s.out && snapshot::opponent_hp(s.p.playerId) == 0) snapshot::set_opponent_hp(s.p.playerId, 1);
    advance_barrier(now);
}

bool waiting() { return g_active && g_waitYear >= 0; }
int waiting_year() { return g_waitYear; }
unsigned waiting_seconds() { return g_waitYear >= 0 ? (unsigned)((GetTickCount64() - g_waitSince) / 1000) : 0; }

std::vector<PeerView> view() {
    std::vector<PeerView> v;
    for (const PeerState& s : g_peers) v.push_back({s.p.name, s.boards.count(g_waitYear) > 0, s.out});
    return v;
}

void stop_waiting() {
    if (g_waitYear < 0 || g_boardsInstalled) return;
    logf("sync: year %d: not waiting any longer", g_waitYear);
    g_forceRelease = true;  // friends without a board this year keep their previous one
}

} // namespace nl::sync
