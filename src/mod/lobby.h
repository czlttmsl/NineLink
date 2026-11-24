#pragma once
// Multiplayer lobby with two interchangeable transports:
//   Mode::Steam  -- friends-only Steam lobbies (no server needed, both sides need Steam)
//   Mode::Server -- NineLink lobby/relay server (ninelink_server), address in ninelink.ini
// The room model (members, kings, ready, host start) is the same for both.
// Everything runs from the main thread's tick(); network I/O for the server
// mode happens on a background thread and is handed over through a queue.
#include <cstdint>
#include <string>
#include <vector>

namespace nl::lobby {

enum class Mode { Steam, Server };
enum class State { Offline, Idle, Creating, Joining, InLobby, Starting };

struct Member {
    uint64_t id = 0;
    std::string name;
    int king = -1;  // lobby king index 0..8, -1 = not chosen
    bool ready = false;
    bool host = false;
    bool me = false;
};

struct Room {
    uint64_t id = 0;
    std::string name;  // Steam: the friend's name; server: room name
    int players = 0;   // 0 = unknown (Steam friend list does not say)
};

struct StartInfo {
    int seed = 0;
    int myKing = -1;
    std::vector<Member> players;
};

constexpr int kKingCount = 9;  // KingOfNothing .. KingOfTime
constexpr int kMaxPlayers = 8;
const char* king_name(int king);

void tick();  // once per frame, main thread

Mode mode();
void set_mode(Mode m);  // only while not in a room
State state();
const std::string& status();
bool is_host();
const std::vector<Member>& members();
const std::vector<Room>& rooms();
int my_king();
bool my_ready();
bool can_start();  // host: >= 2 players, everyone has a king, every client is ready

void create();
void join(uint64_t room);
void leave();
void invite();  // Steam mode: overlay invite dialog
void set_king(int king);
void set_ready(bool ready);
void start();  // host only
void reconnect();  // server mode: retry the connection now

std::string server_address();  // "host:port" from ninelink.ini

bool take_start(StartInfo& out);  // set once the host has started

// Kings are unique per room: the game looks opponents up by their king.
bool king_taken(int king);  // another member already picked it

// In-game data channel between the room's members (works before and after the start).
// Payloads must not contain TAB / CR / LF. Steam: reliable P2P messages via Steam relay;
// server: relayed by ninelink_server (where `to` is ignored -- everyone else receives it).
uint64_t my_id();
void send_data(uint64_t to, const std::string& payload);  // to = 0: everyone else
bool recv_data(uint64_t& from, std::string& payload);      // next received payload, if any
void end_game();  // the run is over: leave the room so the lobby is usable again

} // namespace nl::lobby
