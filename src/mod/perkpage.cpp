#include "perkpage.h"
#include "gui.h"
#include "loadout.h"
#include "lobby.h"
#include "log.h"
#include "perks.h"

#include <string>

namespace nl::perkpage {
namespace {

using namespace gui;

bool g_open = false;
int g_category = 0;
perks::Perk* g_drag = nullptr;      // perk being dragged
loadout::Slot* g_dragFrom = nullptr;  // grid slot it came from (null: from the hand)
perks::Perk* g_hover = nullptr;

constexpr Color kSlotEdge{0.85f, 0.85f, 0.80f, 1.0f};
constexpr Color kSlotFill{0.06f, 0.07f, 0.07f, 0.95f};
constexpr Color kSlotHot{0.98f, 0.78f, 0.35f, 1.0f};

void DrawPerkIcon(Rect r, perks::Perk* p, float alpha = 1.0f) {
    const text::Tex& t = perks::icon(p);
    if (t.texture) {
        Image(r, t.texture, {1, 1, 1, alpha});
    } else {  // no icon: a tinted plate with the name
        Fill(r, {p->theme[0] * 0.5f, p->theme[1] * 0.5f, p->theme[2] * 0.5f, alpha});
        TextBox({r.x + 4 * U, r.y + r.h / 3, r.w - 8 * U, r.h}, p->name, 14, kText);
    }
}

// Grid geometry: the three regular columns (x = -1..1) are centred; Expander columns grow left.
Rect SlotRect(int x, int y) {
    float s = 118 * U, gap = 28 * U;
    float cx = W / 2 + x * (s + gap), top = 150 * U;
    float cy = top + (loadout::kMaxY - y) * (s + gap);
    return {cx - s / 2, cy, s, s};
}

void DrawGrid() {
    for (loadout::Slot& s : loadout::slots()) {
        if (!s.unlocked) continue;
        Rect r = SlotRect(s.x, s.y);
        bool hot = Inside(r);
        Frame(r, kSlotFill, hot && g_drag ? kSlotHot : kSlotEdge, 3 * U);
        if (s.perk && s.perk != g_drag) {
            float pad = 8 * U;
            DrawPerkIcon({r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad}, s.perk);
            if (s.level != 1) {  // official grid shows the applied count when it is not 1
                Rect badge{r.x + r.w - 46 * U, r.y + r.h - 34 * U, 44 * U, 32 * U};
                Fill(badge, {0, 0, 0, 0.75f});
                Text(badge.x + badge.w / 2, badge.y, badge.h, "x" + std::to_string(s.level), 20, kGold, Center, true);
            }
            if (hot) g_hover = s.perk;
            if (!g_drag && TakePress(r)) {
                g_drag = s.perk;
                g_dragFrom = &s;
            }
            if (RightClick(r)) loadout::clear(s);
        }
    }
}

void DrawHand() {
    const auto& cats = perks::categories(loadout::king());
    if (cats.empty()) return;
    if (g_category >= (int)cats.size()) g_category = 0;
    const perks::Category& cat = cats[g_category];

    // Category paging, like the official left / right category buttons.
    float barY = H - 240 * U;
    if (Button({W / 2 - 300 * U, barY, 60 * U, 44 * U}, "<", cats.size() > 1))
        g_category = (g_category + (int)cats.size() - 1) % (int)cats.size();
    if (Button({W / 2 + 240 * U, barY, 60 * U, 44 * U}, ">", cats.size() > 1))
        g_category = (g_category + 1) % (int)cats.size();
    Text(W / 2, barY, 44 * U, cat.title + "  (" + std::to_string(g_category + 1) + "/" + std::to_string(cats.size()) + ")",
         24, kGold, Center, true);

    std::vector<perks::Perk*> hand;
    for (perks::Perk* p : cat.perks)
        if (!loadout::is_placed(p)) hand.push_back(p);
    if (hand.empty()) {
        Text(W / 2, H - 170 * U, 60 * U, "这一类的特权都已放进格子里了。", 20, kTextDim, Center);
        return;
    }
    float size = 96 * U, gap = 10 * U;
    float maxW = W - 160 * U;
    if (hand.size() * (size + gap) > maxW) size = maxW / hand.size() - gap;  // squeeze long hands
    float total = hand.size() * (size + gap) - gap;
    float x = W / 2 - total / 2, y = H - 170 * U;
    for (perks::Perk* p : hand) {
        Rect r{x, y, size, size};
        bool hot = Inside(r) && !g_drag;
        Rect drawn = hot ? Rect{r.x - 10 * U, r.y - 20 * U, r.w + 20 * U, r.h + 20 * U} : r;  // lift on hover
        if (p != g_drag) DrawPerkIcon(drawn, p);
        if (hot) g_hover = p;
        if (!g_drag && TakePress(r)) {
            g_drag = p;
            g_dragFrom = nullptr;
        }
        x += size + gap;
    }
}

void HandleDrop() {
    if (!g_drag || MouseDown()) return;
    // Released: onto an unlocked slot -> place there (swapping if needed); elsewhere -> back to the hand.
    loadout::Slot* target = nullptr;
    for (loadout::Slot& s : loadout::slots())
        if (s.unlocked && Inside(SlotRect(s.x, s.y))) target = &s;
    if (target) {
        if (target != g_dragFrom) {
            perks::Perk* displaced = target->perk;
            int fromX = g_dragFrom ? g_dragFrom->x : 0, fromY = g_dragFrom ? g_dragFrom->y : 0;
            bool fromGrid = g_dragFrom != nullptr;
            loadout::place(*target, g_drag);
            if (displaced && fromGrid)
                if (loadout::Slot* back = loadout::at(fromX, fromY)) loadout::place(*back, displaced);
        }
    } else if (g_dragFrom) {
        loadout::clear(*g_dragFrom);
    }
    g_drag = nullptr;
    g_dragFrom = nullptr;
}

} // namespace

void open(int k) {
    if (k < 0) return;
    loadout::select(k, -1);
    g_open = true;
    g_category = 0;
    g_drag = nullptr;
    g_dragFrom = nullptr;
}

bool is_open() { return g_open; }

void draw() {
    g_hover = nullptr;
    Fill({0, 0, W, H}, {0.05f, 0.04f, 0.04f, 0.97f});

    // Top bar.
    if (Button({40 * U, 28 * U, 150 * U, 54 * U}, "< 返回")) g_open = false;
    if (Button({W - 190 * U, 28 * U, 150 * U, 54 * U}, "完成 >", true, true)) g_open = false;
    std::string title = std::string(lobby::king_name(loadout::king())) + "特权";
    Text(W / 2 - 30 * U, 28 * U, 54 * U, title, 34, kText, Center, true);
    if (Button({W / 2 + 120 * U, 34 * U, 96 * U, 42 * U}, "重置", true, false, 20)) loadout::reset();

    // Save slots, right side.
    for (int i = 0; i < loadout::kSaveSlots; ++i) {
        Rect r{W - 210 * U, 300 * U + i * 66 * U, 170 * U, 52 * U};
        if (Button(r, "槽位" + std::to_string(i + 1), true, loadout::save_slot() == i, 22)) loadout::select(loadout::king(), i);
    }

    DrawGrid();
    DrawHand();

    // Hovered (or dragged) perk: name in green, description below -- under the grid as in the original.
    perks::Perk* info = g_drag ? g_drag : g_hover;
    float infoY = SlotRect(0, loadout::kMinY).y + SlotRect(0, loadout::kMinY).h + 40 * U;
    if (info) {
        Text(W / 2, infoY, 46 * U, info->name, 28, kGreen, Center, true);
        TextBox({W / 2 - 480 * U, infoY + 52 * U, 960 * U, 90 * U}, info->desc, 22, kText, Center);
    } else {
        Text(W / 2, infoY, 46 * U, "把下方的特权拖进格子里", 24, kTextDim, Center);
        Text(W / 2, infoY + 52 * U, 40 * U, "拖出格子或右键可以取回。所有特权在联机里全部解锁。", 20, kTextDim, Center);
    }

    // The dragged perk follows the mouse.
    if (g_drag) {
        float s = 104 * U;
        DrawPerkIcon({MouseX() - s / 2, MouseY() - s / 2, s, s}, g_drag, 0.9f);
    }
    HandleDrop();
}

} // namespace nl::perkpage
