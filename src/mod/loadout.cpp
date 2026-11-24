#include "loadout.h"
#include "lobby.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <optional>

namespace nl::loadout {
namespace {

// Saved layouts: g_saved[king][slot] = list of (x, y, term).
struct Placed {
    int x, y;
    std::string term;
};
std::map<int, std::vector<Placed>> g_saved[kSaveSlots];
std::map<int, int> g_activeSlot;  // king -> save slot last used
bool g_loaded = false;

int g_king = -1, g_slot = 0;
std::vector<Slot> g_grid;
std::vector<std::optional<int>> g_fixed;
std::vector<bool> g_applied;

int idx(const Slot* s) { return (int)(s - g_grid.data()); }

void build_grid() {
    g_grid.clear();
    for (int y = kMaxY; y >= kMinY; --y)
        for (int x = kMinX; x <= kMaxX; ++x) {
            Slot s;
            s.x = x;
            s.y = y;
            s.unlocked = x >= -1;
            g_grid.push_back(s);
        }
}

void set_level(Slot& s, int n) { s.level = g_fixed[idx(&s)] ? *g_fixed[idx(&s)] : n; }
void add_level(Slot& s, int n) { s.level = g_fixed[idx(&s)] ? *g_fixed[idx(&s)] : s.level + n; }

bool g_moderate = false;

void on_calc(Slot& s, int lvl);

void boost(Slot* t, int lvl) {
    if (!t || !t->unlocked) return;
    int old = t->level;
    add_level(*t, lvl);
    int delta = t->level - old;
    if (delta != 0 && t->perk && g_applied[idx(t)]) on_calc(*t, delta);
}

// Perk.OnCalculatePerkLevel and the overrides of the grid-shaping perks.
void on_calc(Slot& s, int lvl) {
    perks::Perk* p = s.perk;
    if (g_moderate && p->owner == perks::kGenericOwner) set_level(s, 2);
    const std::string& c = p->cls;
    if (c == "Leftist") boost(at(s.x - 1, s.y), lvl);
    else if (c == "Verticalist") { boost(at(s.x, s.y + 1), lvl); boost(at(s.x, s.y - 1), lvl); }
    else if (c == "Horizontalist") { boost(at(s.x - 1, s.y), lvl); boost(at(s.x + 1, s.y), lvl); }
}

bool authoritarian_can_affect(const perks::Perk* p) {
    if (!p) return true;
    if (p->owner & perks::kJokerOwner) return false;
    return !(p->owner & perks::kMultiplayerOwner);
}

} // namespace

void recalc() {
    // Expander in the leftmost regular column opens the two extra slots of its row.
    for (Slot& s : g_grid) {
        if (s.x >= -1) continue;
        Slot* edge = at(-1, s.y);
        bool open = edge && edge->perk && edge->perk->cls == "Expander";
        if (!open && s.perk) s.perk = nullptr;  // closed again: its perk returns to the hand
        s.unlocked = open;
    }

    g_fixed.assign(g_grid.size(), std::nullopt);
    g_applied.assign(g_grid.size(), false);
    for (Slot& s : g_grid) s.level = 0;

    g_moderate = false;
    bool authoritarian = false;
    for (Slot& s : g_grid)
        if (s.unlocked && s.perk) {
            if (s.perk->cls == "Moderate") g_moderate = true;
            if (s.perk->cls == "Authoritarian") authoritarian = true;
        }
    if (authoritarian) {
        Slot* topLeft = nullptr;  // smallest x, then largest y
        for (Slot& s : g_grid)
            if (s.unlocked && (!topLeft || s.x < topLeft->x || (s.x == topLeft->x && s.y > topLeft->y))) topLeft = &s;
        if (topLeft && authoritarian_can_affect(topLeft->perk)) g_fixed[idx(topLeft)] = 3;
    }

    for (Slot& s : g_grid) {
        if (!s.unlocked || !s.perk) continue;
        if (s.perk->cls == "Moderate") set_level(s, 1);
        else add_level(s, 1);
        g_applied[idx(&s)] = true;
        on_calc(s, s.level);
    }
}

void select(int k, int slot) {
    load();
    int want = slot < 0 ? (g_activeSlot.count(k) ? g_activeSlot[k] : 0) : slot;
    if (k == g_king && want == g_slot && !g_grid.empty()) return;  // already showing it
    g_king = k;
    g_slot = want;
    g_activeSlot[k] = g_slot;
    build_grid();
    for (const Placed& pl : g_saved[g_slot][k]) {
        Slot* s = at(pl.x, pl.y);
        perks::Perk* p = perks::by_term(pl.term);
        if (s && p) s->perk = p;
    }
    recalc();
    if (slot >= 0) save();  // an explicit save-slot switch: remember it as the active one
}

int king() { return g_king; }
int save_slot() { return g_slot; }
std::vector<Slot>& slots() { return g_grid; }

Slot* at(int x, int y) {
    for (Slot& s : g_grid)
        if (s.x == x && s.y == y) return &s;
    return nullptr;
}

bool is_placed(const perks::Perk* p) {
    for (const Slot& s : g_grid)
        if (s.perk == p) return true;
    return false;
}

void place(Slot& s, perks::Perk* p) {
    if (!s.unlocked) return;
    for (Slot& o : g_grid)
        if (o.perk == p) o.perk = nullptr;  // a perk can only be in one slot
    s.perk = p;
    recalc();
    save();
}

void clear(Slot& s) {
    s.perk = nullptr;
    recalc();
    save();
}

void reset() {
    for (Slot& s : g_grid) s.perk = nullptr;
    recalc();
    save();
}

std::vector<Entry> entries() {
    std::vector<Entry> out;
    for (const Slot& s : g_grid)
        if (s.unlocked && s.perk) out.push_back({s.x, s.y, s.perk->term, s.level});
    return out;
}

std::vector<Entry> entries_for(int k) {
    if (k == g_king) return entries();  // the grid being edited: no reload, no save
    int prevKing = g_king, prevSlot = g_slot;
    select(k, -1);
    std::vector<Entry> out = entries();
    if (prevKing >= 0 && prevKing != k) select(prevKing, prevSlot);
    return out;
}

std::vector<std::pair<int, int>> unlocked_positions() {
    std::vector<std::pair<int, int>> out;
    for (const Slot& s : g_grid)
        if (s.unlocked) out.push_back({s.x, s.y});
    return out;
}

void save() {
    if (g_king < 0) return;
    auto& list = g_saved[g_slot][g_king];
    list.clear();
    for (const Slot& s : g_grid)
        if (s.perk) list.push_back({s.x, s.y, s.perk->term});
    g_activeSlot[g_king] = g_slot;

    FILE* f = nullptr;
    if (_wfopen_s(&f, (mod_dir() + L"\\ninelink_perks.txt").c_str(), L"wb") != 0 || !f) return;
    fputs("# NineLink perk loadouts: king slot active x,y,term ...\n", f);
    for (int k = 0; k < lobby::kKingCount; ++k)
        for (int sl = 0; sl < kSaveSlots; ++sl) {
            auto it = g_saved[sl].find(k);
            bool active = g_activeSlot.count(k) && g_activeSlot[k] == sl;
            if ((it == g_saved[sl].end() || it->second.empty()) && !active) continue;
            fprintf(f, "%d %d %d", k, sl, active ? 1 : 0);
            if (it != g_saved[sl].end())
                for (const Placed& p : it->second) fprintf(f, " %d,%d,%s", p.x, p.y, p.term.c_str());
            fputc('\n', f);
        }
    fclose(f);
}

void load() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (mod_dir() + L"\\ninelink_perks.txt").c_str(), L"rb") != 0 || !f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        int k, sl, active, n = 0;
        if (sscanf_s(line, "%d %d %d%n", &k, &sl, &active, &n) < 3) continue;
        if (k < 0 || k >= lobby::kKingCount || sl < 0 || sl >= kSaveSlots) continue;
        if (active) g_activeSlot[k] = sl;
        auto& list = g_saved[sl][k];
        char* p = line + n;
        char* ctx = nullptr;
        for (char* tok = strtok_s(p, " \r\n", &ctx); tok; tok = strtok_s(nullptr, " \r\n", &ctx)) {
            int x, y, used = 0;
            if (sscanf_s(tok, "%d,%d,%n", &x, &y, &used) >= 2 && used > 0) list.push_back({x, y, tok + used});
        }
    }
    fclose(f);
}

} // namespace nl::loadout
