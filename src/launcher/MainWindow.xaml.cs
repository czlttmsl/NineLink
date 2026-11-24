using System.Diagnostics;
using System.IO;
using System.Windows;

namespace NineLink.Launcher;

public partial class MainWindow : Window
{
    readonly string _root = AppContext.BaseDirectory;
    GameInstall? _game;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += (_, _) => Detect();
    }

    void Detect()
    {
        _game = GameLocator.Find();
        if (_game is null)
        {
            GameText.Text = "没有找到 9 Kings（Steam 应用 2784470）。";
            LaunchButton.IsEnabled = false;
            return;
        }
        GameText.Text = $"游戏：{_game.Exe}";

        var supported = GameLocator.ReadSupported(Path.Combine(_root, "game.ver"));
        string current = $"Steam 构建号 {_game.BuildId} / GameAssembly 0x{_game.GameAssemblyStamp:X8}";
        if (supported is null)
            BuildText.Text = $"{current}（缺少 game.ver，无法核对）";
        else if (supported.GameAssemblyStamp == _game.GameAssemblyStamp)
            BuildText.Text = $"{current} — 与本 mod 适配的版本一致 ✓";
        else
            BuildText.Text = $"{current}\n本 mod 适配：构建号 {supported.BuildId} / 0x{supported.GameAssemblyStamp:X8}。" +
                             "游戏已更新，mod 按名字查找函数，多数情况下仍可用，但可能出错。";
        Log("就绪。");
    }

    async void Launch_Click(object sender, RoutedEventArgs e)
    {
        if (_game is null) return;
        LaunchButton.IsEnabled = false;
        Log("正在启动游戏并注入 mod…");
        var psi = new ProcessStartInfo(Path.Combine(_root, "ninelink_loader.exe"),
            $"--game \"{_game.Exe}\" --dll \"{Path.Combine(_root, "ninelink.dll")}\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };
        try
        {
            using var p = Process.Start(psi)!;
            string? line;
            while ((line = await p.StandardOutput.ReadLineAsync()) is not null) Log(line);
            await p.WaitForExitAsync();
            Log(p.ExitCode == 0 ? "游戏已启动，mod 已加载。" : $"失败（退出码 {p.ExitCode}）。");
        }
        catch (Exception ex)
        {
            Log("启动失败：" + ex.Message);
        }
        LaunchButton.IsEnabled = true;
    }

    void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        string dir = Path.Combine(_root, "log");
        Directory.CreateDirectory(dir);
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true });
    }

    void Log(string s)
    {
        LogBox.AppendText($"[{DateTime.Now:HH:mm:ss}] {s}\n");
        LogBox.ScrollToEnd();
    }
}
