#pragma once
// A king's perk loadout on the multiplayer perk grid, with the level rules of
// MultiplayerHubPerkSlotsManager.CalculatePerks (reverse-engineered):
//   * every placed perk is level 1 (applied once); the Moderate perk itself is 1;
//   * Moderate in the grid: generic perks are applied exactly 2 times;
//   * Authoritarian: the top-left unlocked slot is fixed at 3 (generic / king perks only);
//   * Leftist adds its level to the perk on its left, Verticalist to the perks above and
//     below, Horizontalist to both sides; a boosted perk that already applied its own
//     effect re-applies it with the difference (so modifiers chain);
//   * Expander in the leftmost column opens two extra slots further left in its row.
#include "perks.h"

#include <string>
#include <vector>

namespace nl::loadout {

constexpr int kMinX = -3, kMaxX = 1;  // columns -3..-2 are Expander-only
constexpr int kMinY = -1, kMaxY = 1;  // y grows upwards
constexpr int kSaveSlots = 3;

struct Slot {
    int x = 0, y = 0;
    bool unlocked = false;
    perks::Perk* perk = nullptr;
    int level = 0;  // times the perk is applied
};

struct Entry {  // what the run needs per placed perk
    int x, y;
    std::string term;
    int level;
};

// The grid currently being edited (king + save slot).
void select(int lobbyKing, int saveSlot);
int king();
int save_slot();
std::vector<Slot>& slots();          // all positions, row-major from the top-left
Slot* at(int x, int y);
bool is_placed(const perks::Perk* p);

void place(Slot& s, perks::Perk* p);  // replaces whatever was there
void clear(Slot& s);
void reset();                          // empty the grid
void recalc();                         // levels + Expander slots; called by the mutators

std::vector<Entry> entries();                          // placed perks of the current grid
std::vector<Entry> entries_for(int lobbyKing);         // the remembered active slot of a king
std::vector<std::pair<int, int>> unlocked_positions();  // for RunDefinition.PerkUnlockedSlots

void save();  // ninelink_perks.txt beside the DLL; called by the mutators
void load();

} // namespace nl::loadout
