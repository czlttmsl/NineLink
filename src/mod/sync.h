#pragma once
// Synchronous friend PvP, year by year:
//   1. a player finishes preparing and starts the battle (Wave.StartWave) -- NineLink holds the
//      battle back, captures that player's board and sends it to everyone in the room;
//   2. once every friend still in the game has sent their board for this year, each friend's
//      board goes into their run record and the opponent kingdom is rebuilt from it;
//   3. the battle is released. Everyone fights the boards their friends just built.
// Lives travel with the boards: each player's own game is the authority on their own lives.
#include <cstdint>
#include <string>
#include <vector>

namespace nl::sync {

struct Peer {
    uint64_t id;       // lobby member id (Steam ID / server client id)
    int playerId;      // their MultiplayerRunData.playerId in this run
    std::string name;  // display name
    int king;          // lobby king index
};

bool install();                           // resolve + hook Wave.StartWave (init thread)
void begin(const std::vector<Peer>& peers);  // a lobby game just started (main thread)
void end();                               // the run is over (main thread)
bool active();
void tick();  // every frame, main thread

// Waiting screen.
bool waiting();
int waiting_year();
unsigned waiting_seconds();
struct PeerView {
    std::string name;
    bool ready;  // their board for this year has arrived
    bool out;    // eliminated or left
};
std::vector<PeerView> view();
void stop_waiting();  // start anyway: friends who are not ready keep last year's board

} // namespace nl::sync
