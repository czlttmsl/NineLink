using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace NineLink.Launcher;

public sealed record UpdateInfo(Version Version, string Tag, string Notes, string ZipName, string ZipUrl, string SumsUrl, long Size);

// Self-update from GitHub Releases.
// A release must carry two assets (release.ps1 makes them):
//   NineLink-<version>.zip   the player files (NineLink.exe, ninelink_loader.exe, ninelink.dll, ...)
//   SHA256SUMS.txt           "<sha256>  <file name>" per line; the zip is refused if it does not match
public static class Updater
{
    public const string Repo = "czlttmsl/NineLink";
    public const string ReleasesPage = $"https://github.com/{Repo}/releases";

    // Files that belong to the player and are never overwritten by an update.
    static readonly HashSet<string> Keep = new(StringComparer.OrdinalIgnoreCase) { "ninelink.ini" };

    public static Version Current { get; } = ParseVersion(
        Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? "0.0.0") ?? new Version(0, 0, 0);

    static HttpClient NewClient(TimeSpan timeout)
    {
        var http = new HttpClient { Timeout = timeout };
        http.DefaultRequestHeaders.UserAgent.ParseAdd($"NineLink-Launcher/{Current}");
        http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return http;
    }

    // "v0.1.2", "0.1.2-beta", "0.1.2+abcdef" -> 0.1.2
    static Version? ParseVersion(string s)
    {
        s = s.Trim().TrimStart('v', 'V');
        int cut = s.IndexOfAny(new[] { '-', '+', ' ' });
        if (cut >= 0) s = s[..cut];
        return Version.TryParse(s, out var v) ? v : null;
    }

    // Returns the latest release if it is newer than this launcher, otherwise null.
    // Network problems also return null: the launcher must keep working offline.
    public static async Task<UpdateInfo?> CheckAsync()
    {
        try
        {
            using var http = NewClient(TimeSpan.FromSeconds(8));
            // NINELINK_UPDATE_API: for testing against a local copy of the release JSON.
            string api = Environment.GetEnvironmentVariable("NINELINK_UPDATE_API")
                         ?? $"https://api.github.com/repos/{Repo}/releases/latest";
            using var doc = JsonDocument.Parse(await http.GetStringAsync(api));
            var root = doc.RootElement;
            string tag = root.GetProperty("tag_name").GetString() ?? "";
            var ver = ParseVersion(tag);
            if (ver is null || ver <= Current) return null;

            string? zipName = null, zipUrl = null, sumsUrl = null; long size = 0;
            foreach (var a in root.GetProperty("assets").EnumerateArray())
            {
                string name = a.GetProperty("name").GetString() ?? "";
                string url = a.GetProperty("browser_download_url").GetString() ?? "";
                if (name.Equals("SHA256SUMS.txt", StringComparison.OrdinalIgnoreCase)) sumsUrl = url;
                else if (name.StartsWith("NineLink-", StringComparison.OrdinalIgnoreCase) &&
                         name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) &&
                         !name.Contains("server", StringComparison.OrdinalIgnoreCase))
                { zipName = name; zipUrl = url; size = a.GetProperty("size").GetInt64(); }
            }
            if (zipName is null || zipUrl is null || sumsUrl is null) return null;
            string notes = root.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "";
            return new UpdateInfo(ver, tag, notes, zipName, zipUrl, sumsUrl, size);
        }
        catch
        {
            return null;
        }
    }

    public static bool GameRunning() => Process.GetProcessesByName("9Kings").Length > 0;

    // Downloads, verifies and installs the update into `root`, then restarts the launcher.
    // Throws with a readable message on failure; the old files are restored if anything goes wrong mid-way.
    public static async Task ApplyAsync(UpdateInfo u, string root, IProgress<string> log)
    {
        string work = Path.Combine(Path.GetTempPath(), "NineLink-update-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(work);
        try
        {
            using var http = NewClient(TimeSpan.FromMinutes(5));

            log.Report($"正在下载 {u.ZipName}（{u.Size / 1024.0 / 1024.0:0.0} MB）…");
            string zip = Path.Combine(work, u.ZipName);
            await using (var src = await http.GetStreamAsync(u.ZipUrl))
            await using (var dst = File.Create(zip))
                await src.CopyToAsync(dst);

            log.Report("正在校验文件…");
            string sums = await http.GetStringAsync(u.SumsUrl);
            string? expected = sums.Split('\n')
                .Select(l => l.Trim().Split((char[]?)null, 2, StringSplitOptions.RemoveEmptyEntries))
                .Where(p => p.Length == 2 && p[1].TrimStart('*').Equals(u.ZipName, StringComparison.OrdinalIgnoreCase))
                .Select(p => p[0].ToLowerInvariant())
                .FirstOrDefault();
            string actual;
            await using (var fs = File.OpenRead(zip))
                actual = Convert.ToHexString(await SHA256.HashDataAsync(fs)).ToLowerInvariant();
            if (expected is null || expected != actual)
                throw new InvalidDataException("下载的文件校验失败（可能损坏或不完整），已取消更新。");

            string unpacked = Path.Combine(work, "files");
            ZipFile.ExtractToDirectory(zip, unpacked);
            // Accept zips that wrap everything in one top-level folder.
            var dirs = Directory.GetDirectories(unpacked);
            if (!File.Exists(Path.Combine(unpacked, "NineLink.exe")) && dirs.Length == 1 && Directory.GetFiles(unpacked).Length == 0)
                unpacked = dirs[0];
            if (!File.Exists(Path.Combine(unpacked, "NineLink.exe")))
                throw new InvalidDataException("更新包里没有 NineLink.exe，已取消更新。");

            log.Report("正在安装…");
            Install(unpacked, root);
            log.Report($"已更新到 {u.Tag}，正在重启启动器…");
        }
        finally
        {
            try { Directory.Delete(work, true); } catch { }
        }

        Process.Start(new ProcessStartInfo(Path.Combine(root, "NineLink.exe")) { UseShellExecute = true, WorkingDirectory = root });
        System.Windows.Application.Current.Shutdown();
    }

    // Replaces files one by one. A running exe cannot be overwritten but can be renamed,
    // so every existing file is first moved to "<name>.old" (cleaned up on next start).
    static void Install(string from, string root)
    {
        var done = new List<(string target, string? backup)>();
        try
        {
            foreach (string file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
            {
                string rel = Path.GetRelativePath(from, file);
                string target = Path.Combine(root, rel);
                if (Keep.Contains(rel) && File.Exists(target)) continue;

                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                string? backup = null;
                if (File.Exists(target))
                {
                    backup = target + ".old";
                    File.Move(target, backup, overwrite: true);
                }
                done.Add((target, backup));
                File.Copy(file, target);
            }
        }
        catch (Exception ex)
        {
            for (int i = done.Count - 1; i >= 0; i--)
            {
                var (target, backup) = done[i];
                try
                {
                    if (File.Exists(target)) File.Delete(target);
                    if (backup is not null) File.Move(backup, target);
                }
                catch { }
            }
            if (ex is UnauthorizedAccessException)
                throw new IOException($"没有权限写入 {root}。请把 NineLink 放到可写的文件夹（例如桌面或文档），或去 {ReleasesPage} 手动下载。", ex);
            throw new IOException("安装更新失败，已恢复旧版本：" + ex.Message, ex);
        }
    }

    // Removes "*.old" files left by the previous update.
    public static void CleanupOldFiles(string root)
    {
        try
        {
            foreach (string f in Directory.GetFiles(root, "*.old", SearchOption.AllDirectories))
                try { File.Delete(f); } catch { }
        }
        catch { }
    }
}
