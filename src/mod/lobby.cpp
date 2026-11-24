// Lobby front end: shared room state + dispatch to the active backend.
#include "lobby_internal.h"
#include "log.h"

#include <windows.h>
#include <cstdio>

namespace nl::lobby {
namespace detail {

Shared S;

void set_status(const std::string& s) {
    if (s != S.status) logf("lobby: %s", s.c_str());
    S.status = s;
}

void on_started(int seed) {
    if (S.state == State::Starting) return;
    S.start = {};
    S.start.seed = seed;
    S.start.myKing = S.myKing;
    S.start.players = S.members;
    S.startPending = true;
    S.state = State::Starting;
    set_status("房主已开始游戏！");
}

} // namespace detail

using namespace detail;

namespace {
Mode g_mode = Mode::Steam;
const Backend& B() { return g_mode == Mode::Steam ? steam_backend() : server_backend(); }
bool in_room() { return S.state == State::InLobby || S.state == State::Starting || S.state == State::Joining || S.state == State::Creating; }
} // namespace

const char* king_name(int k) {
    static const char* names[kKingCount] = {"虚无之王", "法术之王", "鲜血之王", "贪婪之王", "进步之王",
                                            "岩石之王", "游牧之王", "自然之王", "时间之王"};
    return (k >= 0 && k < kKingCount) ? names[k] : "未选择";
}

void tick() {
    static bool first = true;
    if (first) {
        first = false;
        if (config_wants_server()) set_mode(Mode::Server);
    }
    B().tick();
}

Mode mode() { return g_mode; }

void set_mode(Mode m) {
    if (m == g_mode || in_room()) return;
    g_mode = m;
    S.rooms.clear();
    S.members.clear();
    S.state = State::Offline;
    S.status.clear();
    B().activate();
}

State state() { return S.state; }
const std::string& status() { return S.status; }
bool is_host() { return S.host; }
const std::vector<Member>& members() { return S.members; }
const std::vector<Room>& rooms() { return S.rooms; }
int my_king() { return S.myKing; }
bool my_ready() { return S.myReady; }

bool can_start() {
    if (S.state != State::InLobby || !S.host || S.members.size() < 2) return false;
    for (size_t i = 0; i < S.members.size(); ++i) {
        const Member& m = S.members[i];
        if (m.king < 0 || (!m.host && !m.ready)) return false;
        for (size_t j = 0; j < i; ++j)
            if (S.members[j].king == m.king) return false;  // two players on one king
    }
    return true;
}

bool king_taken(int king) {
    for (const Member& m : S.members)
        if (!m.me && m.king == king) return true;
    return false;
}

uint64_t my_id() { return B().me(); }

void send_data(uint64_t to, const std::string& payload) { B().send(to, payload); }

bool recv_data(uint64_t& from, std::string& payload) {
    if (S.inbox.empty()) return false;
    from = S.inbox.front().first;
    payload = std::move(S.inbox.front().second);
    S.inbox.pop_front();
    return true;
}

void end_game() {
    if (S.state != State::Starting) return;
    leave();
    S.inbox.clear();
}

void create() {
    if (S.state == State::Idle) B().create();
}
void join(uint64_t room) {
    if (S.state == State::Idle) B().join(room);
}
void leave() {
    B().leave();
    S.members.clear();
    S.myReady = false;
    S.host = false;
}
void invite() { B().invite(); }

void set_king(int king) {
    if (S.state != State::InLobby || king_taken(king)) return;
    S.myKing = king;
    S.myReady = false;  // changing your king un-readies you
    B().publish_me();
}

void set_ready(bool ready) {
    if (S.state != State::InLobby || S.myKing < 0) return;
    S.myReady = ready;
    B().publish_me();
}

void start() {
    if (can_start()) B().start();
}

void reconnect() { B().reconnect(); }

bool take_start(StartInfo& out) {
    if (!S.startPending) return false;
    S.startPending = false;
    out = S.start;
    return true;
}

} // namespace nl::lobby
