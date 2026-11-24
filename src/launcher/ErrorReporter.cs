using System.IO;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace NineLink.Launcher;

// Sends error reports from the mod's logs (log\ninelink_*.log) to the NineLink reporter
// (reporter/ in this repo, a Cloudflare Worker that files them as GitHub issues).
//
// * Only with the player's consent (report.cfg: "yes" / "no"; asked once).
// * Each log file is scanned once (log\reported.txt), only after the game has closed.
// * Private data is removed before sending: file paths, Steam / lobby IDs, the Windows
//   user name and the player name from ninelink.ini. A copy of everything sent is kept in
//   log\sent_reports\ so the player can see it.
public static class ErrorReporter
{
    public const string Endpoint = "https://ninelink-reporter.REPLACE_ME.workers.dev/report";

    const int MaxGroupsPerLog = 3, MaxLinesPerGroup = 10, ContextBefore = 30, ContextAfter = 5;

    static readonly Regex ErrorLine = new(
        @"native exception|managed exception|C\+\+ exception|exception while|\bfailed\b|\bnot found\b|" +
        @"\bmissing\b|\bfaulted\b|\bthrew\b|\bis null\b|returned null|UI disabled",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);
    static readonly Regex NotAnError = new(@"exports resolved, 0 missing|without error", RegexOptions.Compiled);
    static readonly Regex Stamp = new(@"^\[\s*[\d.]+\]\s*", RegexOptions.Compiled);

    public static string ConsentFile(string root) => Path.Combine(root, "report.cfg");

    // null = never asked
    public static bool? Consent(string root)
    {
        try
        {
            string v = File.ReadAllText(ConsentFile(root)).Trim().ToLowerInvariant();
            return v == "yes" ? true : v == "no" ? false : null;
        }
        catch { return null; }
    }

    public static void SetConsent(string root, bool yes) => File.WriteAllText(ConsentFile(root), yes ? "yes" : "no");

    public sealed record Report(string version, string signature, string headline, string[] errors,
                                string context, string gameBuild, string os, string logName);

    // Scans logs not yet scanned, sends what it finds. Returns the number of reports sent.
    public static async Task<int> RunAsync(string root, string gameBuild, IProgress<string> log)
    {
        string logDir = Path.Combine(root, "log");
        if (!Directory.Exists(logDir)) return 0;
        string doneFile = Path.Combine(logDir, "reported.txt");
        var done = File.Exists(doneFile)
            ? new HashSet<string>(File.ReadAllLines(doneFile), StringComparer.OrdinalIgnoreCase)
            : new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        var redact = Redactor(root);
        var reports = new List<Report>();
        var scanned = new List<string>();
        foreach (var f in new DirectoryInfo(logDir).GetFiles("ninelink_*.log").OrderBy(f => f.Name))
        {
            if (done.Contains(f.Name)) continue;
            string[] lines;
            try
            {
                using var fs = new FileStream(f.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var sr = new StreamReader(fs, Encoding.UTF8);
                lines = sr.ReadToEnd().Split('\n').Select(l => redact(l.TrimEnd('\r'))).ToArray();
            }
            catch { continue; }
            reports.AddRange(Analyze(lines, f.Name, gameBuild));
            scanned.Add(f.Name);
        }
        if (scanned.Count == 0) return 0;

        int sent = 0;
        bool allOk = true;
        if (reports.Count > 0)
        {
            log.Report($"在日志里发现 {reports.Count} 类错误，正在发送错误报告…");
            string sentDir = Path.Combine(logDir, "sent_reports");
            Directory.CreateDirectory(sentDir);
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            http.DefaultRequestHeaders.UserAgent.ParseAdd($"NineLink-Launcher/{Updater.Current}");
            string url = Environment.GetEnvironmentVariable("NINELINK_REPORT_URL") ?? Endpoint;
            foreach (var r in reports)
            {
                string body = JsonSerializer.Serialize(r);
                try
                {
                    var res = await http.PostAsync(url, new StringContent(body, Encoding.UTF8, "application/json"));
                    if (!res.IsSuccessStatusCode) { allOk = false; continue; }
                    sent++;
                    File.WriteAllText(Path.Combine(sentDir, $"{DateTime.Now:yyyyMMdd-HHmmss}_{sent}.json"),
                        JsonSerializer.Serialize(r, new JsonSerializerOptions { WriteIndented = true,
                            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
                }
                catch { allOk = false; }
            }
            log.Report(sent > 0 ? $"已发送 {sent} 份错误报告，谢谢！" : "错误报告暂时发送失败，下次启动再试。");
        }
        // Mark as scanned only when everything went out, so a network failure retries next time.
        if (allOk) File.AppendAllLines(doneFile, scanned);
        return sent;
    }

    // Groups the error lines of one log by their normalized text ("signature").
    public static IEnumerable<Report> Analyze(string[] lines, string logName, string gameBuild)
    {
        var groups = new Dictionary<string, List<int>>();
        for (int i = 0; i < lines.Length; i++)
        {
            string l = lines[i];
            if (!ErrorLine.IsMatch(l) || NotAnError.IsMatch(l)) continue;
            string sig = Normalize(l);
            if (!groups.TryGetValue(sig, out var idx)) groups[sig] = idx = new List<int>();
            idx.Add(i);
        }
        string os = Environment.OSVersion.VersionString;
        foreach (var (sig, idx) in groups.Take(MaxGroupsPerLog))
        {
            int first = idx[0];
            int from = Math.Max(0, first - ContextBefore), to = Math.Min(lines.Length, first + ContextAfter + 1);
            string ctx = string.Join("\n", lines[from..to]);
            if (ctx.Length > 15000) ctx = ctx[^15000..];
            yield return new Report(
                Updater.Current.ToString(3), sig, sig.Length > 200 ? sig[..200] : sig,
                idx.Take(MaxLinesPerGroup).Select(i => lines[i].Length > 500 ? lines[i][..500] : lines[i]).ToArray(),
                ctx, gameBuild, os, logName);
        }
    }

    // "[  42.8] run: StartNewGame: native exception 0xC0000005 at 00007FF..." -> "run: StartNewGame: native exception # at #"
    static string Normalize(string line)
    {
        string s = Stamp.Replace(line, "");
        s = Regex.Replace(s, @"\b0x[0-9A-Fa-f]+\b|\b[0-9A-Fa-f]{8,16}\b", "#");
        s = Regex.Replace(s, @"\d+", "N");
        return s.Trim();
    }

    static Func<string, string> Redactor(string root)
    {
        var names = new List<string>();
        if (Environment.UserName.Length >= 2) names.Add(Environment.UserName);
        try
        {
            string ini = Path.Combine(root, "ninelink.ini");
            if (File.Exists(ini))
                foreach (var l in File.ReadAllLines(ini))
                    if (l.StartsWith("name=", StringComparison.OrdinalIgnoreCase))
                    {
                        string n = l[5..].Split(';')[0].Trim();
                        if (n.Length >= 2) names.Add(n);
                    }
        }
        catch { }
        var path = new Regex(@"[A-Za-z]:\\[^\s""'<>|]*");
        var ids = new Regex(@"\b\d{15,20}\b");
        return line =>
        {
            line = path.Replace(line, m => "<path>\\" + Path.GetFileName(m.Value.TrimEnd('\\')));
            line = ids.Replace(line, "<id>");
            foreach (var n in names) line = line.Replace(n, "<name>", StringComparison.OrdinalIgnoreCase);
            return line;
        };
    }
}
