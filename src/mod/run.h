#pragma once
#include "loadout.h"
#include "lobby.h"

#include <string>
#include <vector>

namespace nl::run {

bool init();  // resolve game methods; init thread

// Starts a fresh run for this player: the king they picked in the lobby and the
// lobby's shared seed. Difficulty is fixed internally (PvP has no difficulty) and
// the game's own king-selection screen (difficulty / speed) is bypassed.
// The perk loadout goes in through RunDefinition.Perks, which
// KingManager.GetAllKingActivePerks prefers over the saved loadouts during a run.
// Main thread only. Returns false if the game refused.
bool start(const lobby::StartInfo& info, const std::vector<loadout::Entry>& perks,
           const std::vector<std::pair<int, int>>& unlockedSlots);

// The perks the run will use for a king: the player's own single-player loadout
// (KingSettings active loadout), as display names. Read-only; cached for a couple of seconds.
const std::vector<std::string>& own_perks(int lobbyKing);

void tick(const std::string& scene);
bool mp_active();  // a NineLink multiplayer-mode run is in progress  // every frame: restores the king's own loadout after the run applied ours
std::string active_scene();
void diag();  // debug: log run-start state  // main thread only; "" if unknown

std::string to_utf8(void* il2cppString);

} // namespace nl::run
