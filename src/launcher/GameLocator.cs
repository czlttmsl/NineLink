using System.IO;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace NineLink.Launcher;

public sealed record GameInstall(string Exe, string Dir, string BuildId, uint GameAssemblyStamp);

public sealed record SupportedBuild(string BuildId, uint GameAssemblyStamp, string Label);

public static class GameLocator
{
    public const string AppId = "2784470";

    // Finds 9 Kings through Steam's registry key and library folders.
    public static GameInstall? Find()
    {
        string? steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
        if (steam is null) return null;
        steam = steam.Replace('/', '\\');

        var libraries = new List<string> { steam };
        string vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (File.Exists(vdf))
            foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                libraries.Add(m.Groups[1].Value.Replace(@"\\", @"\"));

        foreach (string lib in libraries.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            string manifest = Path.Combine(lib, "steamapps", $"appmanifest_{AppId}.acf");
            if (!File.Exists(manifest)) continue;
            string text = File.ReadAllText(manifest);
            string installDir = Field(text, "installdir") ?? "9 Kings";
            string dir = Path.Combine(lib, "steamapps", "common", installDir);
            string exe = Path.Combine(dir, "9Kings.exe");
            if (!File.Exists(exe)) continue;
            return new GameInstall(exe, dir, Field(text, "buildid") ?? "?",
                PeTimestamp(Path.Combine(dir, "GameAssembly.dll")));
        }
        return null;
    }

    // game.ver beside the launcher: the build this release was made against.
    public static SupportedBuild? ReadSupported(string file)
    {
        if (!File.Exists(file)) return null;
        var kv = File.ReadAllLines(file)
            .Select(l => l.Split('=', 2))
            .Where(p => p.Length == 2)
            .ToDictionary(p => p[0].Trim(), p => p[1].Trim());
        uint stamp = kv.TryGetValue("gameassembly_stamp", out var s) ? Convert.ToUInt32(s, 16) : 0;
        return new SupportedBuild(kv.GetValueOrDefault("buildid", "?"), stamp, kv.GetValueOrDefault("label", ""));
    }

    static string? Field(string acf, string key)
    {
        var m = Regex.Match(acf, $"\"{key}\"\\s+\"([^\"]*)\"", RegexOptions.IgnoreCase);
        return m.Success ? m.Groups[1].Value : null;
    }

    static uint PeTimestamp(string file)
    {
        if (!File.Exists(file)) return 0;
        using var fs = File.OpenRead(file);
        using var br = new BinaryReader(fs);
        fs.Position = 0x3C;
        int pe = br.ReadInt32();
        fs.Position = pe + 8;
        return br.ReadUInt32();
    }
}
