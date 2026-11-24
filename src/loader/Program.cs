// ninelink_loader.exe -- starts 9Kings.exe and injects ninelink.dll.
//
//   ninelink_loader.exe --game "<...>\9Kings.exe" [--dll "<...>\ninelink.dll"] [--timeout 90]
//
// Exit code 0 = injected and the module shows up in the game process.
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace NineLink.Loader;

internal static class Program
{
    const string SteamAppId = "2784470";
    static bool s_unityLog;

    static int Main(string[] args)
    {
        string? game = null;
        string dll = Path.Combine(AppContext.BaseDirectory, "ninelink.dll");
        int timeout = 90;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--game": game = args[++i]; break;
                case "--dll": dll = args[++i]; break;
                case "--timeout": timeout = int.Parse(args[++i]); break;
                case "--unitylog": s_unityLog = true; break;  // Unity's own Player log -> log\unity_<time>.log
            }
        }
        if (game is null || !File.Exists(game)) return Fail($"game exe not found: {game}");
        dll = Path.GetFullPath(dll);
        if (!File.Exists(dll)) return Fail($"mod dll not found: {dll}");

        try
        {
            return Run(game, dll, TimeSpan.FromSeconds(timeout));
        }
        catch (Exception e)
        {
            return Fail(e.ToString());
        }
    }

    static int Run(string game, string dll, TimeSpan timeout)
    {
        string gameDir = Path.GetDirectoryName(game)!;

        // Without steam_appid.txt a Steamworks game started outside Steam
        // restarts itself through Steam and our process handle goes stale.
        string appIdFile = Path.Combine(gameDir, "steam_appid.txt");
        if (!File.Exists(appIdFile))
        {
            File.WriteAllText(appIdFile, SteamAppId);
            Log($"[*] wrote {appIdFile}");
        }

        if (Process.GetProcessesByName(Path.GetFileNameWithoutExtension(game)).Length > 0)
            return Fail("9 Kings is already running -- close it first");

        // ShellExecute: the game must not inherit our console (it would print Unity's log into it).
        var psi = new ProcessStartInfo(game) { WorkingDirectory = gameDir, UseShellExecute = true };
        if (s_unityLog)
        {
            string logDir = Path.Combine(Path.GetDirectoryName(dll)!, "log");
            Directory.CreateDirectory(logDir);
            string logFile = Path.Combine(logDir, $"unity_{DateTime.Now:yyyyMMdd-HHmmss}.log");
            psi.Arguments = $"-logFile \"{logFile}\"";
            Log($"[*] Unity log -> {logFile}");
        }
        var p = Process.Start(psi)
                ?? throw new InvalidOperationException("Process.Start returned null");
        Log($"[*] started pid {p.Id}");

        // Wait until the IL2CPP runtime is loaded and the game window exists:
        // by then il2cpp_init has run and the domain is safe to attach to.
        var sw = Stopwatch.StartNew();
        while (true)
        {
            if (p.HasExited) return Fail($"game exited early (code {p.ExitCode})");
            if (sw.Elapsed > timeout) return Fail("timed out waiting for the game window");
            p.Refresh();
            if (p.MainWindowHandle != IntPtr.Zero && HasModule(p, "GameAssembly.dll")) break;
            Thread.Sleep(250);
        }
        Log($"[*] game window up after {sw.Elapsed.TotalSeconds:F1}s, injecting {dll}");

        Inject(p, dll);

        p.Refresh();
        if (!HasModule(p, Path.GetFileName(dll))) return Fail("LoadLibraryW ran but the module is not in the game");
        Log("[+] injected. mod log -> log\\ninelink_<time>.log");
        return 0;
    }

    static bool HasModule(Process p, string name)
    {
        try
        {
            foreach (ProcessModule m in p.Modules)
                if (string.Equals(m.ModuleName, name, StringComparison.OrdinalIgnoreCase)) return true;
        }
        catch (Win32Exception) { /* process still starting */ }
        catch (InvalidOperationException) { }
        return false;
    }

    static void Inject(Process p, string dll)
    {
        IntPtr h = OpenProcess(PROCESS_ALL_ACCESS, false, p.Id);
        if (h == IntPtr.Zero) throw new Win32Exception();
        try
        {
            byte[] path = Encoding.Unicode.GetBytes(dll + "\0");
            IntPtr mem = VirtualAllocEx(h, IntPtr.Zero, (nuint)path.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (mem == IntPtr.Zero) throw new Win32Exception();
            if (!WriteProcessMemory(h, mem, path, (nuint)path.Length, out _)) throw new Win32Exception();

            // kernel32 sits at the same address in every process of a boot session.
            IntPtr loadLibrary = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
            IntPtr th = CreateRemoteThread(h, IntPtr.Zero, 0, loadLibrary, mem, 0, out _);
            if (th == IntPtr.Zero) throw new Win32Exception();
            WaitForSingleObject(th, 30_000);
            GetExitCodeThread(th, out uint code);
            CloseHandle(th);
            VirtualFreeEx(h, mem, 0, MEM_RELEASE);
            Log($"[*] remote LoadLibraryW returned 0x{code:X8}");
            if (code == 0) throw new InvalidOperationException("LoadLibraryW failed inside the game");
        }
        finally
        {
            CloseHandle(h);
        }
    }

    static void Log(string s) => Console.WriteLine(s);

    static int Fail(string s)
    {
        Console.WriteLine("[-] " + s);
        return 1;
    }

    const uint PROCESS_ALL_ACCESS = 0x1F0FFF;
    const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000, PAGE_READWRITE = 0x04;

    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, nuint size, uint type, uint protect);
    [DllImport("kernel32", SetLastError = true)] static extern bool VirtualFreeEx(IntPtr h, IntPtr addr, nuint size, uint type);
    [DllImport("kernel32", SetLastError = true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, nuint size, out nuint written);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attr, nuint stack, IntPtr start, IntPtr param, uint flags, out uint tid);
    [DllImport("kernel32", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetExitCodeThread(IntPtr h, out uint code);
    [DllImport("kernel32", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr mod, string name);
}
