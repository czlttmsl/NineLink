// Server backend: talks to ninelink_server over TCP.
//
// Protocol (UTF-8, one message per line, fields separated by TAB, sub-fields by 0x1F):
//   client -> server                       server -> client
//   HELLO  <ver> <name>                    WELCOME <clientId>
//   LIST                                   ROOMS  {<id> 1F <name> 1F <players>}*
//   CREATE <roomName>                      ROOM   <roomId> <hostId> {<id> 1F <name> 1F <king> 1F <ready>}*
//   JOIN   <roomId>                        LEFT   <reason>
//   LEAVE                                  START  <seed>
//   SET    <king> <ready>                  DATA   <fromId> <payload>
//   START                                  ERROR  <message>
//   DATA   <payload>                       PONG
//   PING
#include "lobby_internal.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace nl::lobby::detail {
namespace {

constexpr const char* kProtocol = "1";

enum class Conn { Down, Connecting, Up };

std::atomic<Conn> g_conn{Conn::Down};
std::atomic<SOCKET> g_sock{INVALID_SOCKET};
std::mutex g_inMu, g_sendMu;
std::deque<std::string> g_inbox;
std::string g_connError;  // written by the net thread before it sets Down

std::string g_host = "127.0.0.1", g_port = "27700", g_iniName;
bool g_configLoaded = false;
bool g_wantServer = false;  // ini mode=server: open on the server tab
bool g_autoCreate = false;  // ini debug_autocreate=1: create a room right after connecting (testing)
bool g_active = false;  // the server tab is (or was) shown: keep the connection alive
bool g_welcomed = false;
bool g_helloSent = false;
uint64_t g_me = 0;
ULONGLONG g_lastAttempt = 0, g_lastList = 0, g_lastPing = 0;
ULONGLONG g_retryDelay = 0;

// ---------------------------------------------------------------- config

void load_config() {
    if (g_configLoaded) return;
    g_configLoaded = true;
    std::wstring path = mod_dir() + L"\\ninelink.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        // First run: write a default file so people can find the setting.
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
            fputs("; NineLink settings\r\n"
                  "; server = address:port of the ninelink_server lobby/relay server\r\n"
                  "server=127.0.0.1:27700\r\n"
                  "; name = your name on the server (empty = your Steam name)\r\n"
                  "name=\r\n"
                  "; mode = steam | server  (which tab the lobby opens on)\r\n"
                  "mode=steam\r\n",
                  f);
            fclose(f);
        }
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "server" && !v.empty()) {
            size_t colon = v.rfind(':');
            if (colon != std::string::npos) {
                g_host = v.substr(0, colon);
                g_port = v.substr(colon + 1);
            } else {
                g_host = v;
            }
        } else if (k == "name") {
            g_iniName = v;
        } else if (k == "mode") {
            g_wantServer = v == "server";
        } else if (k == "debug_autocreate") {
            g_autoCreate = v == "1";
        }
    }
    fclose(f);
}

std::string clean(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r' || c == 0x1F) c = ' ';
    return s;
}

std::string my_name() {
    if (!g_iniName.empty()) return clean(g_iniName);
    std::string n = steam_persona();
    if (!n.empty()) return clean(n);
    wchar_t buf[64];
    DWORD len = 64;
    if (GetComputerNameW(buf, &len)) return clean(narrow(std::wstring(buf, len)));
    return "Player";
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

// ---------------------------------------------------------------- network thread

void net_thread(std::string host, std::string port) {
    static bool wsa = false;
    if (!wsa) {
        WSADATA d;
        wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        g_connError = "找不到服务器地址 " + host;
        g_conn = Conn::Down;
        return;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = res; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) {
        g_connError = "连接不上服务器 " + host + ":" + port;
        g_conn = Conn::Down;
        return;
    }
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof nodelay);
    g_sock = s;
    g_conn = Conn::Up;

    std::string buf;
    char chunk[4096];
    for (;;) {
        int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf.erase(0, nl + 1);
            std::lock_guard<std::mutex> lk(g_inMu);
            g_inbox.push_back(std::move(line));
        }
        if (buf.size() > (1 << 20)) break;  // a runaway line: drop the connection
    }
    {
        std::lock_guard<std::mutex> lk(g_sendMu);
        g_sock = INVALID_SOCKET;
    }
    closesocket(s);
    g_connError = "与服务器的连接断开了";
    g_conn = Conn::Down;
}

void send_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_sendMu);
    SOCKET s = g_sock;
    if (s == INVALID_SOCKET) return;
    std::string out = line + "\n";
    const char* p = out.data();
    int left = (int)out.size();
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n <= 0) return;  // the reader thread notices the dead socket
        p += n;
        left -= n;
    }
}

void connect_now() {
    if (g_conn != Conn::Down) return;
    load_config();
    g_lastAttempt = GetTickCount64();
    g_welcomed = false;
    g_helloSent = false;
    g_conn = Conn::Connecting;
    S.state = State::Offline;
    set_status("正在连接服务器 " + g_host + ":" + g_port + " ……");
    std::thread(net_thread, g_host, g_port).detach();
}

// ---------------------------------------------------------------- messages

void handle(const std::string& line) {
    std::vector<std::string> f = split(line, '\t');
    const std::string& cmd = f[0];
    if (cmd == "WELCOME" && f.size() >= 2) {
        g_me = _strtoui64(f[1].c_str(), nullptr, 10);
        g_welcomed = true;
        g_retryDelay = 0;
        S.state = State::Idle;
        g_lastList = 0;
        set_status("已连接服务器 " + g_host + "。创建房间或加入一个房间。");
    } else if (cmd == "ROOMS") {
        if (S.state != State::Idle) return;
        S.rooms.clear();
        for (size_t i = 1; i < f.size(); ++i) {
            std::vector<std::string> r = split(f[i], 0x1F);
            if (r.size() < 3) continue;
            S.rooms.push_back({_strtoui64(r[0].c_str(), nullptr, 10), r[1], atoi(r[2].c_str())});
        }
    } else if (cmd == "ROOM" && f.size() >= 3) {
        uint64_t host = _strtoui64(f[2].c_str(), nullptr, 10);
        bool entering = S.state == State::Creating || S.state == State::Joining;
        S.members.clear();
        for (size_t i = 3; i < f.size(); ++i) {
            std::vector<std::string> m = split(f[i], 0x1F);
            if (m.size() < 4) continue;
            Member mm;
            mm.id = _strtoui64(m[0].c_str(), nullptr, 10);
            mm.name = m[1];
            mm.king = atoi(m[2].c_str());
            mm.ready = m[3] == "1";
            mm.host = mm.id == host;
            mm.me = mm.id == g_me;
            S.members.push_back(mm);
        }
        S.host = host == g_me;
        if (entering) {
            S.state = State::InLobby;
            S.myReady = false;
            set_status(S.host ? "房间已创建，让朋友在服务器房间列表里加入。" : "已加入房间，请选择国王并准备。");
            if (S.myKing >= 0) send_line("SET\t" + std::to_string(S.myKing) + "\t0");
        }
    } else if (cmd == "LEFT") {
        S.state = State::Idle;
        S.members.clear();
        S.host = false;
        S.myReady = false;
        g_lastList = 0;
        set_status(f.size() >= 2 && !f[1].empty() ? f[1] : "已离开房间。");
    } else if (cmd == "START" && f.size() >= 2) {
        on_started(atoi(f[1].c_str()));
    } else if (cmd == "ERROR") {
        if (S.state == State::Creating || S.state == State::Joining) S.state = State::Idle;
        set_status(f.size() >= 2 ? f[1] : "服务器报错");
    } else if (cmd == "DATA" && f.size() >= 3) {
        // In-game sync payload from another member (the PvP sync layer reads S.inbox).
        size_t at = line.find('\t', 5);  // after "DATA\t<fromId>"
        S.inbox.emplace_back(_strtoui64(f[1].c_str(), nullptr, 10), line.substr(at + 1));
    }
}

// ---------------------------------------------------------------- backend

void create();

void tick() {
    load_config();
    ULONGLONG now = GetTickCount64();
    Conn c = g_conn;

    if (c == Conn::Down) {
        if (S.state != State::Offline) {  // just lost it
            S.state = State::Offline;
            S.members.clear();
            S.rooms.clear();
            S.host = false;
        }
        if (!g_connError.empty()) set_status(g_connError + "。" + std::to_string((int)(g_retryDelay / 1000)) + " 秒后重试……");
        if (g_active && now - g_lastAttempt > g_retryDelay) {
            g_retryDelay = g_retryDelay ? std::min(g_retryDelay * 2, (ULONGLONG)30000) : 3000;
            connect_now();
        }
        return;
    }
    if (c == Conn::Connecting) return;

    if (!g_welcomed && !g_helloSent) {
        send_line(std::string("HELLO\t") + kProtocol + "\t" + my_name());
        g_helloSent = true;
    }

    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_inMu);
        lines.swap(g_inbox);
    }
    for (const std::string& l : lines) handle(l);

    if (g_autoCreate && S.state == State::Idle) {
        g_autoCreate = false;
        logf("lobby: debug_autocreate -- creating a room");
        create();
        return;
    }
    if (S.state == State::Idle && now - g_lastList > 3000) {
        g_lastList = now;
        send_line("LIST");
    }
    if (now - g_lastPing > 5000) {
        g_lastPing = now;
        send_line("PING");
    }
}

void create() {
    S.state = State::Creating;
    set_status("正在创建房间……");
    send_line("CREATE\t" + my_name() + " 的房间");
}

void join(uint64_t room) {
    S.state = State::Joining;
    set_status("正在加入房间……");
    send_line("JOIN\t" + std::to_string(room));
}

void leave() {
    send_line("LEAVE");
    if (g_conn == Conn::Up && g_welcomed) S.state = State::Idle;
    g_lastList = 0;
    set_status("已离开房间。");
}

void invite() {}  // server rooms are joined from the room list

void publish_me() { send_line("SET\t" + std::to_string(S.myKing) + "\t" + (S.myReady ? "1" : "0")); }

void start() {
    send_line("START");
    set_status("正在开始游戏……");
}

void activate() {
    g_active = true;
    g_retryDelay = 0;
    g_lastAttempt = 0;
}

void reconnect() {
    g_active = true;
    g_retryDelay = 0;
    g_lastAttempt = 0;
    g_connError.clear();
    connect_now();
}

void send(uint64_t, const std::string& payload) { send_line("DATA\t" + clean(payload)); }

uint64_t me() { return g_me; }

} // namespace

bool config_wants_server() {
    load_config();
    return g_wantServer;
}

const Backend& server_backend() {
    static const Backend b{tick, create, join, leave, invite, publish_me, start, activate, reconnect, send, me};
    return b;
}

} // namespace nl::lobby::detail

namespace nl::lobby {
std::string server_address() {
    detail::load_config();
    return detail::g_host + ":" + detail::g_port;
}
} // namespace nl::lobby
