#include "ui.h"
#include "gui.h"
#include "loadout.h"
#include "lobby.h"
#include "log.h"
#include "perkpage.h"
#include "perks.h"
#include "run.h"
#include "snapshot.h"
#include "sync.h"
#include "unity.h"

#include <windows.h>
#include <atomic>
#include <string>

namespace nl::ui {
namespace {

using namespace gui;
using OnGUI_t = void (*)(void*, const MethodInfo*);

OnGUI_t g_origOnGUI = nullptr;
std::atomic<bool> g_firstCall{true};
bool g_open = false;
bool g_startError = false;

// ---- keep clicks on our panels away from the game's own UI (EventSystem off while open) ----
const MethodInfo *g_esCurrent = nullptr, *g_setEnabled = nullptr;
void* g_disabledEs = nullptr;

void BlockGameInput(bool block) {
    if (!g_esCurrent || !g_setEnabled) return;
    using GetObj_t = void* (*)(const MethodInfo*);
    using SetBool_t = void (*)(void*, bool, const MethodInfo*);
    if (block) {
        void* es = ((GetObj_t)il2cpp::method_pointer(g_esCurrent))(g_esCurrent);
        if (es && es != g_disabledEs) {
            ((SetBool_t)il2cpp::method_pointer(g_setEnabled))(es, false, g_setEnabled);
            g_disabledEs = es;
        }
    } else if (g_disabledEs) {
        if (*(void**)((char*)g_disabledEs + 0x10))  // still alive (m_CachedPtr)
            ((SetBool_t)il2cpp::method_pointer(g_setEnabled))(g_disabledEs, true, g_setEnabled);
        g_disabledEs = nullptr;
    }
}

// ------------------------------------------------------------------ lobby screens

void DrawTabs(Rect p) {
    float x = p.x + p.w - 40 * U - 2 * 200 * U - 12 * U, y = p.y + 28 * U;
    bool steam = lobby::mode() == lobby::Mode::Steam;
    if (Button({x, y, 200 * U, 50 * U}, "Steam 好友", true, steam, 22)) lobby::set_mode(lobby::Mode::Steam);
    if (Button({x + 212 * U, y, 200 * U, 50 * U}, "服务器", true, !steam, 22)) lobby::set_mode(lobby::Mode::Server);
}

void DrawRoomList(Rect p) {
    bool steam = lobby::mode() == lobby::Mode::Steam;
    float x = p.x + 40 * U, y = p.y + 110 * U, w = p.w - 80 * U;

    if (lobby::state() == lobby::State::Offline) {
        Text(x, y, 44 * U, lobby::status(), 22, kTextDim);
        if (!steam) {
            Text(x, y + 56 * U, 40 * U, "服务器地址：" + lobby::server_address() + "（在 ninelink.ini 里修改）", 20,
                 kTextDim);
            if (Button({x, y + 120 * U, 220 * U, 56 * U}, "重新连接")) lobby::reconnect();
        }
        return;
    }

    bool idle = lobby::state() == lobby::State::Idle;
    if (Button({x, y, 280 * U, 60 * U}, "创建房间", idle, false, 24)) lobby::create();
    Text(x + 300 * U, y, 60 * U,
         steam ? "房间仅 Steam 好友可见。" : "服务器 " + lobby::server_address() + " 上的所有人都能看到。", 20,
         kTextDim);

    y += 100 * U;
    Text(x, y, 40 * U, steam ? "好友的房间" : "服务器上的房间", 24, kGold, Left, true);
    y += 50 * U;
    const auto& rooms = lobby::rooms();
    if (rooms.empty())
        Text(x, y, 40 * U,
             steam ? "暂时没有好友在开房间。好友创建房间后会自动出现在这里。" : "服务器上还没有房间，创建一个吧。",
             20, kTextDim);
    for (const auto& r : rooms) {
        Fill({x, y, w, 64 * U}, kRow);
        std::string label = r.name;
        if (r.players > 0) label += "    " + std::to_string(r.players) + "/" + std::to_string(lobby::kMaxPlayers) + " 人";
        Text(x + 20 * U, y, 64 * U, label, 22, kText);
        if (Button({x + w - 170 * U, y + 10 * U, 150 * U, 44 * U}, "加入", idle)) lobby::join(r.id);
        y += 74 * U;
        if (y > p.y + p.h - 150 * U) break;
    }
    if (!idle) Text(x, p.y + p.h - 70 * U, 40 * U, lobby::status(), 22, kTextDim);
}

void DrawRoom(Rect p) {
    const auto& ms = lobby::members();
    bool host = lobby::is_host();

    // Left column: players.
    float lx = p.x + 40 * U, ly = p.y + 96 * U, lw = 520 * U;
    Text(lx, ly, 40 * U, "玩家  " + std::to_string(ms.size()) + "/8", 24, kGold, Left, true);
    ly += 52 * U;
    for (const auto& m : ms) {
        Fill({lx, ly, lw, 76 * U}, m.me ? Color{0.26f, 0.20f, 0.13f, 0.95f} : kRow);
        Text(lx + 18 * U, ly + 6 * U, 34 * U, m.name + (m.host ? "  (房主)" : ""), 22, m.host ? kGold : kText, Left,
             m.me);
        Text(lx + 18 * U, ly + 38 * U, 32 * U, lobby::king_name(m.king), 20, m.king >= 0 ? kText : kTextDim);
        const char* tag = m.host ? (m.king >= 0 ? "已就绪" : "选择中") : (m.ready ? "已准备" : "未准备");
        bool good = m.host ? m.king >= 0 : m.ready;
        Text(lx + lw - 18 * U, ly, 76 * U, tag, 20, good ? kGreen : kTextDim, Right, true);
        ly += 84 * U;
    }

    // Right column: king picker (3 x 3) and the perk page.
    float rx = p.x + 600 * U, ry = p.y + 96 * U;
    Text(rx, ry, 40 * U, "选择你的国王（每人不同）", 24, kGold, Left, true);
    ry += 52 * U;
    float bw = 150 * U, bh = 62 * U, gap = 12 * U;
    for (int k = 0; k < lobby::kKingCount; ++k) {
        Rect b{rx + (k % 3) * (bw + gap), ry + (k / 3) * (bh + gap), bw, bh};
        bool taken = lobby::king_taken(k);  // kings are unique per room
        if (Button(b, lobby::king_name(k), !taken, lobby::my_king() == k, 20)) lobby::set_king(k);
    }
    // Perks follow each player's own progress: the run uses the loadout configured for this
    // king in single-player (game menu: 开始 -> choose the king -> 特权).
    float py = ry + 3 * (bh + gap) + 16 * U, pw = 3 * bw + 2 * gap;
    int myKing = lobby::my_king();
    Text(rx, py, 32 * U, "特权：使用你在单人模式里为该国王配置的特权", 18, kTextDim);
    if (myKing >= 0) {
        const auto& mine = run::own_perks(myKing);
        std::string line;
        for (const std::string& n : mine) line += (line.empty() ? "" : "、") + n;
        if (line.empty()) line = "这个国王目前没有配置特权";
        TextBox({rx, py + 38 * U, pw, 90 * U}, line, 18, mine.empty() ? kTextDim : kGreen, Left);
    }

    // Bottom bar.
    float by = p.y + p.h - 90 * U;
    Text(p.x + 40 * U, by - 50 * U, 40 * U, lobby::status(), 20, kTextDim);
    if (Button({p.x + 40 * U, by, 180 * U, 60 * U}, "离开房间")) lobby::leave();
    if (lobby::mode() == lobby::Mode::Steam && Button({p.x + 240 * U, by, 200 * U, 60 * U}, "邀请好友"))
        lobby::invite();
    Rect main{p.x + p.w - 300 * U, by, 260 * U, 60 * U};
    if (host) {
        if (Button(main, "开始游戏", lobby::can_start(), lobby::can_start(), 26)) lobby::start();
        if (!lobby::can_start())
            Text(main.x + main.w, by - 50 * U, 40 * U,
                 ms.size() < 2 ? "至少需要 2 名玩家" : "等待所有人选好国王并准备", 18, kTextDim, Right);
    } else {
        bool ready = lobby::my_ready();
        if (Button(main, ready ? "取消准备" : "准备", myKing >= 0, ready, 26)) lobby::set_ready(!ready);
        if (myKing < 0) Text(main.x + main.w, by - 50 * U, 40 * U, "先选择一位国王", 18, kTextDim, Right);
    }
}

void DrawWaiting() {
    auto peers = sync::view();
    float w = 620 * U, h = (150 + 52 * (float)peers.size()) * U;
    unsigned secs = sync::waiting_seconds();
    bool canSkip = secs >= 60;
    if (canSkip) h += 80 * U;
    Rect p{W / 2 - w / 2, 120 * U, w, h};
    Frame(p, kPanel, kPanelEdge, 3 * U);
    Text(p.x + w / 2, p.y + 20 * U, 50 * U, "第 " + std::to_string(sync::waiting_year() + 1) + " 年  等待好友完成准备", 28,
         kGold, Center, true);
    Text(p.x + w / 2, p.y + 72 * U, 36 * U, "你的布置已发送，所有人准备好后自动开战（" + std::to_string(secs) + " 秒）", 18,
         kTextDim, Center);
    float y = p.y + 124 * U;
    for (const auto& v : peers) {
        Fill({p.x + 30 * U, y, w - 60 * U, 44 * U}, kRow);
        Text(p.x + 48 * U, y, 44 * U, v.name, 22, kText);
        const char* tag = v.out ? "已出局" : v.ready ? "已准备" : "准备中……";
        Text(p.x + w - 48 * U, y, 44 * U, tag, 20, v.out ? kTextDim : v.ready ? kGreen : kGold, Right, true);
        y += 52 * U;
    }
    if (canSkip &&
        Button({p.x + w / 2 - 200 * U, y + 14 * U, 400 * U, 52 * U}, "不等了，直接开战（未准备的用上一年布置）", true, false, 18))
        sync::stop_waiting();
}

void StartRun(const lobby::StartInfo& si) {
    // Perks: each player's own single-player loadout, which StartNewGame applies by itself.
    std::string own;
    for (const std::string& n : run::own_perks(si.myKing)) own += n + "  ";
    logf("ui: perks for this run (own single-player loadout): %s", own.empty() ? "(none)" : own.c_str());
    try {
        if (!run::start(si, {}, {})) g_startError = true;
        else g_open = false;
    } catch (const Il2CppExceptionWrapper& e) {
        il2cpp::log_exception(e.ex, "StartNewGame");
        g_startError = true;
    } catch (...) {
        logf("ui: StartNewGame threw");
        g_startError = true;
    }
    run::diag();
}

void Draw() {
    // Track the active scene: the button belongs to the main menu only.
    static std::string lastScene, menuScene;
    static bool inRun = false;
    std::string scene = run::active_scene();
    if (scene != lastScene) {
        logf("ui: scene \"%s\" -> \"%s\"", lastScene.c_str(), scene.c_str());
        lastScene = scene;
        if (inRun && scene == menuScene) inRun = false;  // back on the main menu
    }

    run::tick(scene);
    lobby::tick();
    sync::tick();
    // Developer test: debug_autoroom.txt -- in a room, pick king 0 and (host) start once everyone is ready.
    static bool autoRoom = GetFileAttributesW((mod_dir() + L"\\debug_autoroom.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (autoRoom && scene == "MainMenu" && lobby::state() == lobby::State::InLobby) {
        if (lobby::my_king() < 0) lobby::set_king(0);
        if (lobby::is_host() && lobby::can_start()) {
            logf("ui: debug_autoroom -- starting the room");
            lobby::start();
        }
    }
    lobby::StartInfo si;
    bool started = lobby::take_start(si);

    // Developer test: debug_autostart.txt beside the DLL starts a solo run (king 0) 3 s into the main menu.
    static ULONGLONG menuSince = 0;
    static bool autoChecked = false;
    if (scene == "MainMenu") { if (!menuSince) menuSince = GetTickCount64(); }
    else menuSince = 0;
    if (!started && !autoChecked && menuSince && GetTickCount64() - menuSince > 3000) {
        autoChecked = true;
        if (GetFileAttributesW((mod_dir() + L"\\debug_autostart.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            logf("ui: debug_autostart.txt found -- starting a solo test run");
            si = {};
            si.seed = 12345;
            si.myKing = 0;
            started = true;
        }
    }

    if (started) {
        logf("ui: START seed=%d myKing=%d players=%zu", si.seed, si.myKing, si.players.size());
        menuScene = scene;
        StartRun(si);
        inRun = !g_startError;
    }

    {   // Developer test: debug_snapshot.txt runs a snapshot round trip 12 s into a run.
        static ULONGLONG gameplaySince = 0;
        static bool tested = false;
        if (scene == "Gameplay") { if (!gameplaySince) gameplaySince = GetTickCount64(); }
        else gameplaySince = 0;
        if (!tested && gameplaySince && GetTickCount64() - gameplaySince > 12000 &&
            GetFileAttributesW((mod_dir() + L"\\debug_snapshot.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            tested = true;
            snapshot::self_test();
        }
        static int mpLogs = 0;  // experiment: what the run sees of its multiplayer opponent
        if (gameplaySince && mpLogs < 2 && GetTickCount64() - gameplaySince > (mpLogs ? 45000u : 15000u) &&
            GetFileAttributesW((mod_dir() + L"\\debug_mpself.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            ++mpLogs;
            snapshot::log_mp_state();
        }
    }

    if (sync::waiting()) {  // battle held back until every friend has finished preparing
        DrawWaiting();
        BlockGameInput(true);  // the board was sent: no more changes this year
        return;
    }

    bool onMenu = !inRun && (scene == "MainMenu" || scene == menuScene);
    if (!onMenu) {
        BlockGameInput(false);
        return;
    }

    if (Button({W - 230 * U, 20 * U, 210 * U, 56 * U}, g_open ? "关闭联机" : "联机 NineLink", true, g_open, 22))
        g_open = !g_open;
    BlockGameInput(g_open);
    if (!g_open) return;

    Rect p{W / 2 - 560 * U, H / 2 - 380 * U, 1120 * U, 760 * U};
    Frame(p, kPanel, kPanelEdge, 3 * U);
    Text(p.x + 40 * U, p.y + 24 * U, 50 * U, "NineLink 好友联机", 32, kGold, Left, true);

    switch (lobby::state()) {
    case lobby::State::Offline:
    case lobby::State::Idle:
    case lobby::State::Creating:
    case lobby::State::Joining:
        DrawTabs(p);
        DrawRoomList(p);
        break;
    case lobby::State::InLobby:
        DrawRoom(p);
        break;
    case lobby::State::Starting:
        Text(p.x + p.w / 2, p.y + p.h / 2 - 30 * U, 60 * U, g_startError ? "开局失败" : "正在开局……", 36, kGold,
             Center, true);
        if (g_startError) {
            Text(p.x + p.w / 2, p.y + p.h / 2 + 40 * U, 40 * U, "游戏拒绝了开局请求，详情见 log 文件夹里的日志。", 22,
                 kTextDim, Center);
            if (Button({p.x + p.w / 2 - 110 * U, p.y + p.h / 2 + 110 * U, 220 * U, 56 * U}, "离开房间")) {
                g_startError = false;
                lobby::leave();
            }
        }
        break;
    }
}

void HookedOnGUI(void* self, const MethodInfo* m) {
    if (g_origOnGUI) g_origOnGUI(self, m);
    if (g_firstCall.exchange(false)) logf("ui: Core.Game.OnGUI hook is live");
    static int errors = 0;
    gui::begin_frame();
    try {
        Draw();
    } catch (...) {
        // IL2CPP managed exceptions arrive as C++ exceptions; never let one escape into Unity's loop.
        if (errors++ < 5) logf("ui: exception while drawing (#%d)", errors);
    }
    gui::end_frame();
}

} // namespace

bool install() {
    Il2CppClass* game = unity::klass("Assembly-CSharp.dll", "Core", "Game");
    const MethodInfo* onGUI = unity::method(game, "OnGUI", 0);
    if (!onGUI || !gui::init()) {
        logf("ui: required Unity methods missing -- UI disabled");
        return false;
    }
    g_esCurrent = unity::method(unity::klass("UnityEngine.UI.dll", "UnityEngine.EventSystems", "EventSystem"), "get_current", 0);
    g_setEnabled = unity::method(unity::klass("UnityEngine.CoreModule.dll", "UnityEngine", "Behaviour"), "set_enabled", 1);
    g_origOnGUI = (OnGUI_t)unity::swap_method_pointer(onGUI, (void*)&HookedOnGUI);
    logf("ui: Core.Game.OnGUI rerouted (original %p); input blocking %s", (void*)g_origOnGUI,
         g_esCurrent && g_setEnabled ? "on" : "unavailable");
    return true;
}

} // namespace nl::ui
