#pragma once
// The game's perk catalogue (PerkSystem.Perk ScriptableObjects), read straight
// from Core.Game.Perks so names, descriptions and icons match the game exactly.
#include "text.h"

#include <string>
#include <vector>

namespace nl::perks {

struct Perk {
    void* obj = nullptr;     // PerkSystem.Perk
    std::string cls;         // IL2CPP class name: "Moderate", "Authoritarian", "Leftist", ...
    std::string term;        // Perk.Name: the key PerkSlotSettings.perkTerm uses (e.g. "Perk_Holy")
    std::string name, desc;  // localized (Chinese when the game is in Chinese), rich-text tags stripped
    int maxLevel = 1;
    int owner = 0;           // Core.MultipleCardOwnership flags
    bool universal = false;  // a generic perk (shared by all kings)
    float theme[4] = {1, 1, 1, 1};
    void* sprite = nullptr;
    text::Tex icon;          // filled lazily by icon()
};

bool init();  // resolve methods; any thread attached to IL2CPP

constexpr int kGenericOwner = 0x1FF;       // all nine kings: a generic perk
constexpr int kMultiplayerOwner = 0x2000;  // MultipleCardOwnership.Multiplayer
constexpr int kJokerOwner = 0x800;

struct Category {
    std::string title;
    std::vector<Perk*> perks;
};

// Main thread only (localisation + textures):
// The PvP hand for a king, everything unlocked: generic perks, the king's own perks,
// and the multiplayer perks (minus the ranked-only ones).
const std::vector<Category>& categories(int lobbyKing);
Perk* by_term(const std::string& term);
const text::Tex& icon(Perk* p);  // the perk's sprite copied into a standalone texture
void dump_catalog();             // debug: log every perk

} // namespace nl::perks
