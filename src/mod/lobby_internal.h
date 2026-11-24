#pragma once
// Shared lobby state and the backend interface. Only lobby*.cpp include this.
#include "lobby.h"

#include <deque>
#include <utility>

namespace nl::lobby::detail {

struct Shared {
    State state = State::Offline;
    std::string status;
    std::vector<Member> members;
    std::vector<Room> rooms;
    bool host = false;
    int myKing = -1;
    bool myReady = false;
    bool startPending = false;
    StartInfo start;
    std::deque<std::pair<uint64_t, std::string>> inbox;  // received data payloads (from, payload)
};
extern Shared S;

void set_status(const std::string& s);
// Called by a backend when the room reports "started" with this seed.
void on_started(int seed);

struct Backend {
    void (*tick)();  // poll / process network; keeps S.state, S.members, S.rooms, S.host current
    void (*create)();
    void (*join)(uint64_t room);
    void (*leave)();
    void (*invite)();
    void (*publish_me)();  // push S.myKing / S.myReady to the room
    void (*start)();       // host asks the room to start
    void (*activate)();    // this backend became the visible one
    void (*reconnect)();
    void (*send)(uint64_t to, const std::string& payload);
    uint64_t (*me)();
};
const Backend& steam_backend();
const Backend& server_backend();

std::string steam_persona();  // "" until Steam is up
bool config_wants_server();   // ninelink.ini mode=server

} // namespace nl::lobby::detail
