// Steam backend: friends-only Steam lobbies, polled (never touches the game's
// Steamworks.NET callback dispatcher).
#include "lobby_internal.h"
#include "log.h"
#include "patch.h"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace nl::lobby::detail {
namespace {

constexpr uint32_t kAppId = 2784470;
constexpr const char* kTag = "NineLink1";  // lobby data "nl": protocol version

using Iface_t = void* (*)();
using CreateLobby_t = uint64_t (*)(void*, int, int);
using JoinLobby_t = uint64_t (*)(void*, uint64_t);
using LeaveLobby_t = void (*)(void*, uint64_t);
using NumMembers_t = int (*)(void*, uint64_t);
using MemberByIndex_t = uint64_t (*)(void*, uint64_t, int);
using SetLobbyData_t = bool (*)(void*, uint64_t, const char*, const char*);
using GetLobbyData_t = const char* (*)(void*, uint64_t, const char*);
using SetMemberData_t = void (*)(void*, uint64_t, const char*, const char*);
using GetMemberData_t = const char* (*)(void*, uint64_t, uint64_t, const char*);
using GetOwner_t = uint64_t (*)(void*, uint64_t);
using SetJoinable_t = bool (*)(void*, uint64_t, bool);
using RequestLobbyData_t = bool (*)(void*, uint64_t);
using PersonaName_t = const char* (*)(void*);
using FriendName_t = const char* (*)(void*, uint64_t);
using FriendCount_t = int (*)(void*, int);
using FriendByIndex_t = uint64_t (*)(void*, int, int);
using FriendGame_t = bool (*)(void*, uint64_t, void*);
using InviteDialog_t = void (*)(void*, uint64_t);
using GetSteamID_t = uint64_t (*)(void*);
using CallDone_t = bool (*)(void*, uint64_t, bool*);
using CallResult_t = bool (*)(void*, uint64_t, void*, int, int, bool*);
// ISteamNetworkingMessages (in-game data). SteamNetworkingIdentity is a 136-byte struct.
struct Identity { unsigned char raw[136]; };
using SetSteamID64_t = void (*)(Identity*, uint64_t);
using SendToUser_t = int (*)(void*, const Identity*, const void*, uint32_t, int, int);
using ReceiveOnChannel_t = int (*)(void*, int, void**, int);
using AcceptSession_t = bool (*)(void*, const Identity*);
using MsgRelease_t = void (*)(void*);
using InitRelay_t = void (*)(void*);

struct Api {
    void *mm = nullptr, *friends = nullptr, *user = nullptr, *utils = nullptr;
    CreateLobby_t CreateLobby;
    JoinLobby_t JoinLobby;
    LeaveLobby_t LeaveLobby;
    NumMembers_t GetNumLobbyMembers;
    MemberByIndex_t GetLobbyMemberByIndex;
    SetLobbyData_t SetLobbyData;
    GetLobbyData_t GetLobbyData;
    SetMemberData_t SetLobbyMemberData;
    GetMemberData_t GetLobbyMemberData;
    GetOwner_t GetLobbyOwner;
    SetJoinable_t SetLobbyJoinable;
    RequestLobbyData_t RequestLobbyData;
    PersonaName_t GetPersonaName;
    FriendName_t GetFriendPersonaName;
    FriendCount_t GetFriendCount;
    FriendByIndex_t GetFriendByIndex;
    FriendGame_t GetFriendGamePlayed;
    InviteDialog_t ActivateGameOverlayInviteDialog;
    GetSteamID_t GetSteamID;
    CallDone_t IsAPICallCompleted;
    CallResult_t GetAPICallResult;
    void* net = nullptr;  // ISteamNetworkingMessages
    SetSteamID64_t SetSteamID64;
    SendToUser_t SendMessageToUser;
    ReceiveOnChannel_t ReceiveMessagesOnChannel;
    AcceptSession_t AcceptSessionWithUser;
    MsgRelease_t ReleaseMessage;
} A;
bool g_apiReady = false;
bool g_netReady = false;
constexpr int kChannel = 0x4E4C;  // "NL"

#pragma pack(push, 8)
struct LobbyCreated_t {  // k_iCallback 513
    int result;
    uint64_t lobby;
};
struct LobbyEnter_t {  // k_iCallback 504
    uint64_t lobby;
    uint32_t chatPermissions;
    bool locked;
    uint32_t response;  // 1 = success
};
struct FriendGameInfo_t {
    uint64_t gameId;
    uint32_t ip;
    uint16_t gamePort, queryPort;
    uint64_t lobby;
};
#pragma pack(pop)

template <class T> bool load(HMODULE m, T& fn, const char* name) {
    fn = (T)GetProcAddress(m, name);
    if (!fn) logf("steam: %s missing", name);
    return fn != nullptr;
}

void hook_result_fetch(HMODULE steam);  // below

bool init_api() {
    HMODULE m = GetModuleHandleW(L"steam_api64.dll");
    if (!m) return false;
    auto iface = [&](const char* n) { auto f = (Iface_t)GetProcAddress(m, n); return f ? f() : nullptr; };
    A.mm = iface("SteamAPI_SteamMatchmaking_v009");
    A.friends = iface("SteamAPI_SteamFriends_v017");
    A.user = iface("SteamAPI_SteamUser_v023");
    A.utils = iface("SteamAPI_SteamUtils_v010");
    if (!A.mm || !A.friends || !A.user || !A.utils) return false;
    bool ok = true;
    ok &= load(m, A.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    ok &= load(m, A.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    ok &= load(m, A.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    ok &= load(m, A.GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    ok &= load(m, A.GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    ok &= load(m, A.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    ok &= load(m, A.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    ok &= load(m, A.SetLobbyMemberData, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
    ok &= load(m, A.GetLobbyMemberData, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
    ok &= load(m, A.GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    ok &= load(m, A.SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    ok &= load(m, A.RequestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    ok &= load(m, A.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= load(m, A.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ok &= load(m, A.GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    ok &= load(m, A.GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    ok &= load(m, A.GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    ok &= load(m, A.ActivateGameOverlayInviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    ok &= load(m, A.GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    ok &= load(m, A.IsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= load(m, A.GetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
    // Networking messages are optional for the lobby itself (only the in-game sync needs them).
    A.net = iface("SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    g_netReady = A.net && load(m, A.SetSteamID64, "SteamAPI_SteamNetworkingIdentity_SetSteamID64") &&
                 load(m, A.SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser") &&
                 load(m, A.ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel") &&
                 load(m, A.AcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser") &&
                 load(m, A.ReleaseMessage, "SteamAPI_SteamNetworkingMessage_t_Release");
    if (void* utils = iface("SteamAPI_SteamNetworkingUtils_SteamAPI_v004")) {
        // Warm up the relay network now rather than on the first message.
        if (auto relay = (InitRelay_t)GetProcAddress(m, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess"))
            relay(utils);
    }
    logf("steam: P2P messages %s", g_netReady ? "ready" : "UNAVAILABLE");
    hook_result_fetch(m);
    return ok && A.GetSteamID(A.user) != 0;
}

uint64_t g_me = 0, g_lobby = 0, g_call = 0;
ULONGLONG g_joinedAt = 0, g_lastDataAsk = 0;
uint64_t g_joinTarget = 0;  // lobby a join is in flight for
ULONGLONG g_lastRoomScan = 0, g_lastInit = 0;

bool ensure_api() {
    if (g_apiReady) return true;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastInit < 2000) return false;
    g_lastInit = now;
    g_apiReady = init_api();
    if (g_apiReady) g_me = A.GetSteamID(A.user);
    return g_apiReady;
}

std::string lobby_str(const char* key) {
    const char* v = A.GetLobbyData(A.mm, g_lobby, key);
    return v ? v : "";
}

void publish_me() {
    if (!g_lobby) return;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", S.myKing);
    A.SetLobbyMemberData(A.mm, g_lobby, "king", buf);
    A.SetLobbyMemberData(A.mm, g_lobby, "ready", S.myReady ? "1" : "0");
}

void enter_lobby(uint64_t id) {
    g_lobby = id;
    S.state = State::InLobby;
    S.myReady = false;
    publish_me();
    S.host = A.GetLobbyOwner(A.mm, g_lobby) == g_me;
    set_status(S.host ? "房间已创建，邀请好友加入吧。" : "已加入房间，请选择国王并准备。");
}

void refresh_members() {
    S.members.clear();
    uint64_t owner = A.GetLobbyOwner(A.mm, g_lobby);
    S.host = owner == g_me;
    int n = A.GetNumLobbyMembers(A.mm, g_lobby);
    for (int i = 0; i < n; ++i) {
        Member m;
        m.id = A.GetLobbyMemberByIndex(A.mm, g_lobby, i);
        m.me = m.id == g_me;
        m.host = m.id == owner;
        const char* name = m.me ? A.GetPersonaName(A.friends) : A.GetFriendPersonaName(A.friends, m.id);
        m.name = name ? name : "?";
        const char* k = A.GetLobbyMemberData(A.mm, g_lobby, m.id, "king");
        m.king = (k && *k) ? atoi(k) : -1;
        const char* r = A.GetLobbyMemberData(A.mm, g_lobby, m.id, "ready");
        m.ready = r && r[0] == '1';
        S.members.push_back(m);
    }
}

// Friends in 9 Kings who sit in a lobby. That lobby is not necessarily a NineLink room (the game
// can be in a lobby of its own), so each one's data is requested once and the room is listed only
// when it carries the NineLink tag.
void scan_friend_rooms() {
    static std::map<uint64_t, ULONGLONG> requested;  // lobby -> when its data was requested
    ULONGLONG now = GetTickCount64();
    S.rooms.clear();
    const int kImmediate = 4;
    int n = A.GetFriendCount(A.friends, kImmediate);
    for (int i = 0; i < n; ++i) {
        uint64_t f = A.GetFriendByIndex(A.friends, i, kImmediate);
        FriendGameInfo_t gi{};
        if (!A.GetFriendGamePlayed(A.friends, f, &gi)) continue;
        if ((gi.gameId & 0xFFFFFF) != kAppId || !gi.lobby) continue;
        const char* tag = A.GetLobbyData(A.mm, gi.lobby, "nl");
        if (!tag || std::string(tag) != kTag) {
            auto it = requested.find(gi.lobby);
            if (it == requested.end() || now - it->second > 10000) {  // ask (again) for its data
                requested[gi.lobby] = now;
                A.RequestLobbyData(A.mm, gi.lobby);
            }
            continue;
        }
        const char* name = A.GetFriendPersonaName(A.friends, f);
        S.rooms.push_back({gi.lobby, std::string(name ? name : "?") + " 的房间", 0});
    }
}

const char* enter_error(uint32_t r) {  // EChatRoomEnterResponse
    switch (r) {
    case 2: return "房间已经不存在了";
    case 3: return "没有权限加入（房间仅好友可见，或已开始游戏）";
    case 4: return "房间已满";
    case 5: return "Steam 出错了";
    case 6: return "你被这个房间禁止加入";
    case 7: return "受限账号不能加入（Steam 账号需要解除受限）";
    case 10: return "房主屏蔽了你";
    case 11: return "你屏蔽了房主";
    case 15: return "操作太频繁，请稍等几秒再试";
    default: return "加入失败";
    }
}

Identity identity(uint64_t steamId) {
    Identity id{};
    A.SetSteamID64(&id, steamId);
    return id;
}

// Receive P2P messages into S.inbox; accept sessions from room members (a peer's first
// message opens a session that must be accepted -- harmless to repeat for every member).
void pump_messages() {
    if (!g_netReady || !g_lobby) return;
    static ULONGLONG lastAccept = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastAccept > 1000) {
        lastAccept = now;
        for (const Member& m : S.members)
            if (!m.me) {
                Identity id = identity(m.id);
                A.AcceptSessionWithUser(A.net, &id);
            }
    }
    void* msgs[16];
    for (int round = 0; round < 8; ++round) {
        int n = A.ReceiveMessagesOnChannel(A.net, kChannel, msgs, 16);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            char* msg = (char*)msgs[i];
            const char* data = *(const char**)msg;          // m_pData
            int size = *(int*)(msg + 8);                     // m_cbSize
            uint64_t from = *(uint64_t*)(msg + 16 + 8);      // m_identityPeer.m_steamID64
            if (data && size > 0) S.inbox.emplace_back(from, std::string(data, size));
            A.ReleaseMessage(msg);
        }
    }
}

// The game's Steamworks.NET runs in manual-dispatch mode and fetches the result of EVERY
// completed API call (SteamAPI_ManualDispatch_GetAPICallResult) -- after that the result is gone
// for us. So that export is hooked: when the game fetches the result of our pending call, a
// copy is kept here. (Before the hook, joins/creates failed whenever the game got there first.)
using ManualGetResult_t = bool (*)(int pipe, uint64_t call, void* buf, int size, int expected, bool* failed);
patch::Redirect g_resultHook;
void* g_resultFn = nullptr;
struct CapturedResult {
    uint64_t call = 0;
    bool ok = false, failed = true;
    int size = 0;
    unsigned char data[64] = {};
} g_captured;

bool seh_get_result(int pipe, uint64_t call, void* buf, int size, int expected, bool* failed, bool& r) {
    __try { r = ((ManualGetResult_t)g_resultFn)(pipe, call, buf, size, expected, failed); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool HookManualGetResult(int pipe, uint64_t call, void* buf, int size, int expected, bool* failed) {
    g_resultHook.remove();
    bool r = false;
    bool ran = seh_get_result(pipe, call, buf, size, expected, failed, r);
    g_resultHook.install(g_resultFn, (void*)&HookManualGetResult);
    if (ran && call && call == g_call && buf && size > 0 && size <= (int)sizeof g_captured.data) {
        g_captured.call = call;
        g_captured.ok = r;
        g_captured.failed = failed ? *failed : false;
        g_captured.size = size;
        memcpy(g_captured.data, buf, size);
    }
    return r;
}

void hook_result_fetch(HMODULE steam) {
    if (g_resultHook.active) return;
    g_resultFn = (void*)GetProcAddress(steam, "SteamAPI_ManualDispatch_GetAPICallResult");
    bool ok = g_resultFn && g_resultHook.install(g_resultFn, (void*)&HookManualGetResult);
    logf("steam: call-result capture %s", ok ? "on" : "UNAVAILABLE");
}

ULONGLONG g_callStarted = 0;  // when g_call was issued

template <class T> int poll_call(T& out, int callbackId) {  // 0 pending, 1 done, -1 failed
    if (g_captured.call == g_call && g_call) {  // the game fetched it first: use our copy
        if (!g_captured.ok || g_captured.failed || g_captured.size < (int)sizeof out) return -1;
        memcpy(&out, g_captured.data, sizeof out);
        return 1;
    }
    bool failed = false;
    if (A.IsAPICallCompleted(A.utils, g_call, &failed) && !failed &&
        A.GetAPICallResult(A.utils, g_call, &out, sizeof out, callbackId, &failed) && !failed)
        return 1;
    // Not (yet) available: the game may still be about to fetch it. Give up after 10 s.
    return GetTickCount64() - g_callStarted > 10000 ? -1 : 0;
}

bool i_am_member(uint64_t lobby) {
    int n = A.GetNumLobbyMembers(A.mm, lobby);
    for (int i = 0; i < n; ++i)
        if (A.GetLobbyMemberByIndex(A.mm, lobby, i) == g_me) return true;
    return false;
}

void tick() {
    if (!ensure_api()) {
        S.state = State::Offline;
        set_status("Steam 尚未就绪……");
        return;
    }
    ULONGLONG now = GetTickCount64();
    switch (S.state) {
    case State::Offline:
        S.state = State::Idle;
        set_status("创建一个房间，或加入好友的房间。");
        g_lastRoomScan = 0;
        [[fallthrough]];
    case State::Idle:
        if (now - g_lastRoomScan > 2000) {
            g_lastRoomScan = now;
            scan_friend_rooms();
        }
        break;
    case State::Creating: {
        LobbyCreated_t r{};
        int p = poll_call(r, 513);
        if (p < 0 || (p > 0 && r.result != 1)) {
            S.state = State::Idle;
            set_status("创建房间失败（Steam 返回 " + std::to_string(p > 0 ? r.result : -1) + "）。");
        } else if (p > 0) {
            g_lobby = r.lobby;
            A.SetLobbyData(A.mm, g_lobby, "nl", kTag);
            A.SetLobbyData(A.mm, g_lobby, "state", "lobby");
            logf("steam: created lobby %llu (friends only)", (unsigned long long)g_lobby);
            enter_lobby(r.lobby);
        }
        break;
    }
    case State::Joining: {
        if (g_lobby) {  // entered; waiting for the room's data to arrive before checking the tag
            std::string tag = lobby_str("nl");
            if (tag == kTag) {
                logf("steam: lobby data arrived after %.1f s", (now - g_joinedAt) / 1000.0);
                enter_lobby(g_lobby);
            } else if (now - g_lastDataAsk > 2000 && now - g_joinedAt <= 15000) {
                g_lastDataAsk = now;  // nudge Steam for the room's data
                A.RequestLobbyData(A.mm, g_lobby);
            } else if (now - g_joinedAt > 15000) {
                logf("steam: lobby %llu has tag \"%s\" (want %s) -- leaving", (unsigned long long)g_lobby, tag.c_str(), kTag);
                A.LeaveLobby(A.mm, g_lobby);
                g_lobby = 0;
                S.state = State::Idle;
                set_status(tag.empty() ? "这个房间不是 NineLink 房间（可能是游戏自带的房间）。"
                                       : "这个房间是另一个版本的 NineLink 创建的，请双方用同一版本。");
            }
            break;
        }
        LobbyEnter_t r{};
        int p = poll_call(r, 504);
        if (p <= 0 && g_joinTarget && i_am_member(g_joinTarget)) {  // in the member list: we are in
            r.lobby = g_joinTarget;
            r.response = 1;
            p = 1;
            logf("steam: no join result, but we are a member of lobby %llu", (unsigned long long)g_joinTarget);
        }
        if (p < 0 || (p > 0 && r.response != 1)) {
            logf("steam: JoinLobby failed (call %d, response %u)", p, p > 0 ? r.response : 0);
            S.state = State::Idle;
            set_status(std::string("加入房间失败：") + (p > 0 ? enter_error(r.response) : "Steam 没有回应"));
        } else if (p > 0) {
            g_lobby = r.lobby;
            g_joinedAt = now;
            g_lastDataAsk = now;
            A.RequestLobbyData(A.mm, g_lobby);
            set_status("已进入房间，正在读取房间信息……");
            logf("steam: entered lobby %llu, owner %llu, %d member(s), tag \"%s\"", (unsigned long long)g_lobby,
                 (unsigned long long)A.GetLobbyOwner(A.mm, g_lobby), A.GetNumLobbyMembers(A.mm, g_lobby),
                 lobby_str("nl").c_str());
        }
        break;
    }
    case State::InLobby:
        refresh_members();
        if (lobby_str("state") == "start") on_started(atoi(lobby_str("seed").c_str()));
        break;
    case State::Starting:  // in the game: keep the member list current (who is still here)
        if (g_lobby) refresh_members();
        break;
    default:
        break;
    }
    pump_messages();
}

void send(uint64_t to, const std::string& payload) {
    if (!g_netReady || !g_lobby) return;
    const int kReliable = 8, kAutoRestartBrokenSession = 32;
    for (const Member& m : S.members) {
        if (m.me || (to && m.id != to)) continue;
        Identity id = identity(m.id);
        int r = A.SendMessageToUser(A.net, &id, payload.data(), (uint32_t)payload.size(),
                                    kReliable | kAutoRestartBrokenSession, kChannel);
        if (r != 1) logf("steam: send to %llu failed (EResult %d, %zu bytes)", (unsigned long long)m.id, r, payload.size());
    }
}

uint64_t me() { return g_me; }

void create() {
    const int kFriendsOnly = 1;
    g_callStarted = GetTickCount64();
    g_call = A.CreateLobby(A.mm, kFriendsOnly, kMaxPlayers);
    S.state = State::Creating;
    set_status("正在创建房间……");
}

void join(uint64_t lobby) {
    g_lobby = 0;
    g_joinTarget = lobby;
    logf("steam: joining lobby %llu", (unsigned long long)lobby);
    g_callStarted = GetTickCount64();
    g_call = A.JoinLobby(A.mm, lobby);
    S.state = State::Joining;
    set_status("正在加入房间……");
}

void leave() {
    if (g_lobby && g_apiReady) A.LeaveLobby(A.mm, g_lobby);
    g_lobby = 0;
    S.state = g_apiReady ? State::Idle : State::Offline;
    g_lastRoomScan = 0;
    set_status("已离开房间。");
}

void invite() {
    if (g_lobby) A.ActivateGameOverlayInviteDialog(A.friends, g_lobby);
}

void start() {
    char seed[16];
    snprintf(seed, sizeof seed, "%d", (int)((GetTickCount64() ^ g_me) & 0x7FFFFFFF));
    A.SetLobbyJoinable(A.mm, g_lobby, false);
    A.SetLobbyData(A.mm, g_lobby, "seed", seed);
    A.SetLobbyData(A.mm, g_lobby, "state", "start");
    set_status("正在开始游戏……");
}

void activate() { g_lastRoomScan = 0; }
void reconnect() { g_lastInit = 0; }

} // namespace

std::string steam_persona() {
    if (!ensure_api()) return {};
    const char* n = A.GetPersonaName(A.friends);
    return n ? n : "";
}

const Backend& steam_backend() {
    static const Backend b{tick, create, join, leave, invite, publish_me, start, activate, reconnect, send, me};
    return b;
}

} // namespace nl::lobby::detail
