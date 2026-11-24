#pragma once
// Kingdom snapshots for the yearly PvP exchange. A snapshot is the game's own
// Gameplay.GameSnapshot (kings, kingdom areas, policies, hand, ...), captured with
// GameSnapshot.CaptureSnapshot() and turned into JSON with the same Newtonsoft settings
// the official multiplayer uses to upload runs -- so everything is built by game code.
#include <string>
#include <vector>

struct Il2CppClass;

namespace nl::snapshot {

bool init();  // resolve methods (init thread)

// Main thread, during a run. Empty string on failure (details in the log).
std::string capture_json();

// JSON -> a GameSnapshot object (pinned with a GC handle; release with release()).
void* from_json(const std::string& json);
void release(void* snapshot);

// Debug: capture -> JSON -> GameSnapshot round trip, JSON written to log\snapshot_<year>.json.
void self_test();

// Any JSON -> game object of class k (or k[] when asArray), built by Newtonsoft (game code).
// Pinned like from_json; release with release().
void* deserialize(const std::string& json, Il2CppClass* k, bool asArray);

// Opponents for a multiplayer-mode run: builds a MultiplayerSession from per-opponent
// snapshot JSON and installs it as MultiplayerManager.Session. Main thread.
struct Opponent {
    int playerId;
    std::string name;
    int kingOwner;                       // Core.CardOwnership value
    std::vector<std::string> snapshots;  // GameSnapshot JSON, one per year so far
    // The year this opponent's run ended. The game drops an opponent after that year (as for a
    // recorded run that died there), so a live opponent gets a large value until they are out.
    int yearPlayed = 99;
};
bool install_session(const std::vector<Opponent>& opponents);

// Debug: log what the run sees of its multiplayer opponent.
void log_mp_state();

// Wave.PrepareMultiplayerGauntlet(): picks this run's opponents from the Session.
bool prepare_gauntlet();

// While a NineLink multiplayer run is live: MultiplayerManager.SubmitMultiplayerRun is
// redirected to a no-op so nothing is ever sent to the official ranked servers, and the
// official "one rival left -> jump to the final year" rule is switched off. Restored after.
void block_official_submit(bool block);

// Every frame during a multiplayer run: logs each year change (type, attacker, opponent state).
// Also keeps every year's attacking king (WaveProperties.King) set to a live opponent: the game
// fills it lazily, and RefreshMultiplayerMap (opponent board) and PlayerBase.ReceivedLethalDamage
// (opponent castle destroyed -> they lose a life) both look the opponent up by it -- unset, both
// throw, leaving a stale board and an indestructible castle.
void watch_years();
void assign_wave_kings();  // the King part of the above on its own (right after the run starts)

// ---- live opponents (synchronous PvP), main thread ----
// Puts a friend's board for one year into their run record (replacing that year's board if
// there is one). The year is the snapshot's own WaveData.WaveNumber.
bool put_opponent_snapshot(int playerId, const std::string& json);
// Sets a friend's lives in their run record. hp <= 0: they are out (dropped after this year).
void set_opponent_hp(int playerId, int hp);
int opponent_hp(int playerId);  // -1 if unknown
// Wave.RefreshMultiplayerMap: rebuilds the opponent kingdom from the run records. It is async
// (awaits the areas), give it about a second before the battle starts.
bool refresh_opponent_board();
int my_lives();       // PlayerLife.Instance.HealthPoints; -1 if unknown
int current_year();   // GameplayData.Wave; -1 outside a run
std::string year_of(const std::string& json);  // WaveData.WaveNumber as text ("" if missing)
void log_battle_counts(const char* tag);  // debug: Wave troop counters / target collections
bool start_battle();                      // debug: Wave.StartWave, as the start-battle button does

// Every frame during any run: true exactly once when this year's battle begins, i.e. the
// preparation phase (cards / buildings / troops) is over -- the moment to take the snapshot.
// Fills `year` with the year that is about to be fought.
bool battle_started(int& year);

} // namespace nl::snapshot
