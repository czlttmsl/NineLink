// ninelink_server -- lobby + relay server for NineLink (9 Kings friend multiplayer).
//
//   ninelink_server [--port 27700]
//
// Runs on Windows or Linux (.NET 8). Players connect over TCP; nobody needs an
// open port except this server. Protocol: UTF-8 lines, TAB-separated fields,
// 0x1F-separated sub-fields (see src/mod/lobby_server.cpp for the full table).
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace NineLink.Server;

sealed class Client
{
    public required ulong Id;
    public required TcpClient Tcp;
    public required StreamWriter Writer;
    public string Name = "?";
    public bool Welcomed;
    public Room? Room;
    public int King = -1;
    public bool Ready;
    public DateTime LastSeen = DateTime.UtcNow;
    readonly object _sendLock = new();

    public void Send(string line)
    {
        lock (_sendLock)
        {
            try { Writer.Write(line); Writer.Write('\n'); Writer.Flush(); }
            catch { /* the reader loop will notice and clean up */ }
        }
    }
}

sealed class Room
{
    public required ulong Id;
    public required string Name;
    public required Client Host;
    public readonly List<Client> Members = new();
    public bool Started;
}

static class Program
{
    const string Protocol = "1";
    const int MaxPlayers = 8;
    const int MaxLine = 1 << 20;
    const char Sub = '\x1F';

    static readonly object Lock = new();  // guards rooms and membership
    static readonly Dictionary<ulong, Room> Rooms = new();
    static readonly ConcurrentDictionary<ulong, Client> Clients = new();
    static long _nextId = 1000;
    static readonly Random Rng = new();

    static async Task Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);  // room / player names are Chinese
        int port = 27700;
        for (int i = 0; i < args.Length; i++)
            if (args[i] == "--port" && i + 1 < args.Length) port = int.Parse(args[++i]);

        var listener = new TcpListener(IPAddress.IPv6Any, port);
        listener.Server.DualMode = true;  // IPv4 and IPv6
        listener.Start();
        Log($"NineLink server listening on port {port}");
        _ = Task.Run(Reaper);

        while (true)
        {
            var tcp = await listener.AcceptTcpClientAsync();
            _ = Task.Run(() => Serve(tcp));
        }
    }

    static async Task Serve(TcpClient tcp)
    {
        tcp.NoDelay = true;
        var stream = tcp.GetStream();
        var c = new Client
        {
            Id = (ulong)Interlocked.Increment(ref _nextId),
            Tcp = tcp,
            Writer = new StreamWriter(stream, new UTF8Encoding(false)) { NewLine = "\n" },
        };
        Clients[c.Id] = c;
        string peer = tcp.Client.RemoteEndPoint?.ToString() ?? "?";
        try
        {
            using var reader = new StreamReader(stream, new UTF8Encoding(false));
            while (true)
            {
                string? line = await reader.ReadLineAsync();
                if (line is null || line.Length > MaxLine) break;
                c.LastSeen = DateTime.UtcNow;
                Handle(c, line);
            }
        }
        catch (Exception e) when (e is IOException or SocketException or ObjectDisposedException) { }
        finally
        {
            Drop(c, "disconnected");
            Log($"#{c.Id} {c.Name} ({peer}) left");
        }
    }

    static void Handle(Client c, string line)
    {
        string[] f = line.Split('\t');
        string cmd = f[0];
        if (!c.Welcomed && cmd != "HELLO") { c.Send("ERROR\t请先发送 HELLO"); return; }

        switch (cmd)
        {
            case "HELLO":
                if (f.Length < 3 || f[1] != Protocol) { c.Send("ERROR\t客户端版本和服务器不一致，请更新 NineLink"); return; }
                c.Name = Clean(f[2], 32);
                c.Welcomed = true;
                c.Send($"WELCOME\t{c.Id}");
                Log($"#{c.Id} {c.Name} connected from {c.Tcp.Client.RemoteEndPoint}");
                break;

            case "PING":
                c.Send("PONG");
                break;

            case "LIST":
            {
                var sb = new StringBuilder("ROOMS");
                lock (Lock)
                    foreach (var r in Rooms.Values.Where(r => !r.Started))
                        sb.Append('\t').Append(r.Id).Append(Sub).Append(r.Name).Append(Sub).Append(r.Members.Count);
                c.Send(sb.ToString());
                break;
            }

            case "CREATE":
            {
                lock (Lock)
                {
                    LeaveRoom(c, null);
                    var r = new Room { Id = (ulong)Rng.NextInt64(100000, 999999), Name = Clean(f.Length > 1 ? f[1] : c.Name + " 的房间", 40), Host = c };
                    while (Rooms.ContainsKey(r.Id)) r.Id = (ulong)Rng.NextInt64(100000, 999999);
                    Rooms[r.Id] = r;
                    JoinRoom(c, r);
                    Log($"#{c.Id} {c.Name} created room {r.Id} \"{r.Name}\"");
                }
                break;
            }

            case "JOIN":
            {
                lock (Lock)
                {
                    if (f.Length < 2 || !ulong.TryParse(f[1], out ulong id) || !Rooms.TryGetValue(id, out var r))
                    { c.Send("ERROR\t房间不存在了"); return; }
                    if (r.Started) { c.Send("ERROR\t这个房间已经开始游戏了"); return; }
                    if (r.Members.Count >= MaxPlayers) { c.Send("ERROR\t房间已满"); return; }
                    LeaveRoom(c, null);
                    JoinRoom(c, r);
                    Log($"#{c.Id} {c.Name} joined room {r.Id}");
                }
                break;
            }

            case "LEAVE":
                lock (Lock) LeaveRoom(c, "");
                break;

            case "SET":
                lock (Lock)
                {
                    if (c.Room is null || c.Room.Started) return;
                    if (f.Length >= 2 && int.TryParse(f[1], out int king))
                    {
                        king = Math.Clamp(king, -1, 8);
                        // Kings are unique per room (the game looks opponents up by king).
                        if (king < 0 || !c.Room.Members.Any(m => m != c && m.King == king)) c.King = king;
                    }
                    if (f.Length >= 3) c.Ready = f[2] == "1" && c.King >= 0;
                    Broadcast(c.Room);
                }
                break;

            case "START":
                lock (Lock)
                {
                    var r = c.Room;
                    if (r is null || r.Host != c) { c.Send("ERROR\t只有房主可以开始游戏"); return; }
                    if (r.Members.Count < 2) { c.Send("ERROR\t至少需要 2 名玩家"); return; }
                    if (r.Members.Any(m => m.King < 0 || (m != r.Host && !m.Ready))) { c.Send("ERROR\t还有人没选好国王或没准备"); return; }
                    if (r.Members.Select(m => m.King).Distinct().Count() != r.Members.Count) { c.Send("ERROR\t每位玩家要选不同的国王"); return; }
                    r.Started = true;
                    int seed = Rng.Next(1, int.MaxValue);
                    foreach (var m in r.Members) m.Send($"START\t{seed}");
                    Log($"room {r.Id} started with {r.Members.Count} players, seed {seed}");
                }
                break;

            case "DATA":
            {
                // Relay in-game sync payloads to everyone else in the room.
                Room? r;
                lock (Lock) r = c.Room;
                if (r is null || f.Length < 2) return;
                string msg = $"DATA\t{c.Id}\t{line.Substring(5)}";
                List<Client> targets;
                lock (Lock) targets = r.Members.Where(m => m != c).ToList();
                foreach (var m in targets) m.Send(msg);
                break;
            }

            default:
                c.Send($"ERROR\t未知命令 {Clean(cmd, 16)}");
                break;
        }
    }

    // ---- room membership (call with Lock held) ----

    static void JoinRoom(Client c, Room r)
    {
        c.Room = r;
        c.King = -1;
        c.Ready = false;
        r.Members.Add(c);
        Broadcast(r);
    }

    static void LeaveRoom(Client c, string? notify)
    {
        var r = c.Room;
        if (r is null) return;
        r.Members.Remove(c);
        c.Room = null;
        c.Ready = false;
        if (notify is not null) c.Send($"LEFT\t{notify}");
        if (r.Members.Count == 0)
        {
            Rooms.Remove(r.Id);
            Log($"room {r.Id} closed");
            return;
        }
        if (r.Host == c)
        {
            r.Host = r.Members[0];  // host migrates to the longest-standing member
            r.Host.Ready = false;
        }
        Broadcast(r);
    }

    static void Broadcast(Room r)
    {
        var sb = new StringBuilder($"ROOM\t{r.Id}\t{r.Host.Id}");
        foreach (var m in r.Members)
            sb.Append('\t').Append(m.Id).Append(Sub).Append(m.Name).Append(Sub).Append(m.King).Append(Sub).Append(m.Ready ? '1' : '0');
        string msg = sb.ToString();
        foreach (var m in r.Members) m.Send(msg);
    }

    static void Drop(Client c, string why)
    {
        Clients.TryRemove(c.Id, out _);
        lock (Lock) LeaveRoom(c, null);
        try { c.Tcp.Close(); } catch { }
    }

    // Clients ping every 5 s; anyone silent for 30 s is gone.
    static async Task Reaper()
    {
        while (true)
        {
            await Task.Delay(5000);
            var cutoff = DateTime.UtcNow.AddSeconds(-30);
            foreach (var c in Clients.Values.Where(c => c.LastSeen < cutoff).ToList())
            {
                Log($"#{c.Id} {c.Name} timed out");
                Drop(c, "timeout");
            }
        }
    }

    static string Clean(string s, int max)
    {
        var sb = new StringBuilder(s.Length);
        foreach (char ch in s) sb.Append(ch is '\t' or '\n' or '\r' or Sub ? ' ' : ch);
        string t = sb.ToString().Trim();
        if (t.Length == 0) t = "?";
        return t.Length > max ? t[..max] : t;
    }

    static void Log(string s) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {s}");
}
