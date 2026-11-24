using System.Diagnostics;
using System.IO;
using System.Windows;

namespace NineLink.Launcher;

public partial class MainWindow : Window
{
    readonly string _root = AppContext.BaseDirectory;
    GameInstall? _game;
    UpdateInfo? _update;

    public MainWindow()
    {
        InitializeComponent();
        VersionText.Text = "v" + Updater.Current.ToString(3);
        Loaded += async (_, _) =>
        {
            Updater.CleanupOldFiles(_root);
            Detect();
            await CheckForUpdate();
        };
    }

    async Task CheckForUpdate()
    {
        Log("正在检查更新…");
        _update = await Updater.CheckAsync();
        if (_update is null)
        {
            Log("已是最新版本（或暂时无法连接 GitHub）。");
            return;
        }
        string notes = _update.Notes.Trim();
        if (notes.Length > 160) notes = notes[..160] + "…";
        UpdateText.Text = $"发现新版本 {_update.Tag}（当前 v{Updater.Current.ToString(3)}）" +
                          (notes.Length > 0 ? "\n" + notes : "");
        UpdateBanner.Visibility = Visibility.Visible;
        Log($"发现新版本 {_update.Tag}。");
    }

    void UpdateLater_Click(object sender, RoutedEventArgs e) => UpdateBanner.Visibility = Visibility.Collapsed;

    async void Update_Click(object sender, RoutedEventArgs e)
    {
        if (_update is null) return;
        if (Updater.GameRunning())
        {
            MessageBox.Show(this, "请先关闭 9 Kings 再更新（游戏运行时模组文件被占用）。", "NineLink",
                MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
        UpdateButton.IsEnabled = UpdateLaterButton.IsEnabled = LaunchButton.IsEnabled = false;
        try
        {
            await Updater.ApplyAsync(_update, _root, new Progress<string>(Log));
        }
        catch (Exception ex)
        {
            Log("更新失败：" + ex.Message);
            var r = MessageBox.Show(this, ex.Message + "\n\n要打开下载页面手动更新吗？", "NineLink 更新失败",
                MessageBoxButton.YesNo, MessageBoxImage.Warning);
            if (r == MessageBoxResult.Yes)
                Process.Start(new ProcessStartInfo(Updater.ReleasesPage) { UseShellExecute = true });
            UpdateButton.IsEnabled = UpdateLaterButton.IsEnabled = true;
            LaunchButton.IsEnabled = _game is not null;
        }
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
