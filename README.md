# NineLink — 9 Kings 好友联机 mod（仿 CoMeow 架构）

## 联机方式
游戏右上角「联机 NineLink」→ 大厅右上角两个标签：
- **Steam 好友**：好友专用的 Steam 房间，不需要服务器，双方都要开着 Steam。
- **服务器**：连 `ninelink_server`（大厅 + 转发），不需要开端口，也不依赖 Steam 好友关系。
  服务器地址写在 `ninelink.ini`（和 ninelink.dll 同目录，首次运行自动生成）：
  ```
  server=127.0.0.1:27700
  name=          ; 留空 = 用 Steam 名字
  mode=steam     ; 大厅默认打开哪个标签：steam / server
  ```

## 运行服务器
- 本机测试：双击 `dist\server\ninelink_server.exe`（默认端口 27700，`--port` 可改）。
  同一台电脑上的游戏用默认的 `127.0.0.1:27700` 就能连上。
- 部署到 Linux 云服务器（以后有服务器时）：
  ```
  # 本地打包
  dotnet publish src/server/Server.csproj -c Release -r linux-x64 --self-contained true -p:PublishSingleFile=true -o out/linux
  # 拷到服务器后
  chmod +x ninelink_server && ./ninelink_server --port 27700
  ```
  云服务器的防火墙/安全组要放行 TCP 27700。玩家把 `ninelink.ini` 的 `server=` 改成服务器的 IP:27700。

## 同步对战：开发笔记（进行中）
已验证可行的零件：
- 快照：`GameSnapshot.CaptureSnapshot()` → `JsonConvert.SerializeObject(obj, settings)` → JSON（约 24KB / 31ms）
  → `JsonConvert.DeserializeObject(json, Type, settings)` 还原。代码在 `src/mod/snapshot.cpp`。
- 多人模式开局：`StartNewGame(..., isMultiplayer: true, ...)`，开局前把 `MultiplayerManager.Session`
  设成自己构造的 `MultiplayerSession`（对手数据用 JSON 反序列化成 `MultiplayerRunData[]`，全部由游戏代码创建）。
  注意：先 `il2cpp_runtime_class_init(MultiplayerManager)`，再直接写静态区（类指针+0xB8 + 字段偏移），
  `il2cpp_field_static_set_value` 对这个字段无效。
- 调试：`ninelink_loader.exe --unitylog` 打开 Unity 自己的日志（log\unity_*.log）。

关键发现：官方多人模式只有**第 33 年**（WaveOptionsSo+0x2B8）才和其他玩家的快照打"车轮战"
（`Wave.PrepareMultiplayerGauntlet` → `MultiplayerSession.PopulateGauntletEnemies` 按该年份取快照），
平时每年仍是电脑国王。要做每年同步对战，需要让每一年都变成这种对打（下一步研究第 33 年的波次配置和开战流程）。

同步对战（`sync.cpp`）每年的流程：
1. 玩家点开战（`Wave.StartWave` 被拦下）→ 拍下自己的布置（`GameSnapshot` JSON），连同剩余生命发给房间里所有人（`B|年|生命|json`，对方回 `A|年|id` 确认，没确认的每 3 秒重发）。
2. 屏幕显示“等待好友完成准备”，期间屏蔽游戏输入（布置已发出，不能再改）。等 60 秒后可以选择不等。
3. 所有还在局里的好友的布置都到了 → 写进各自的对手记录（`MultiplayerGameSnapshots`，同一年替换），然后调用 `Wave.SpawnWave` 重建对手王国并登记敌军，1.5 秒后放行 `StartWave`。
4. 生命以各自游戏为准：本地把对手打到 0 只会暂时显示为 1，等对方下次报告；对方没命了或离开房间（15 秒）就算出局（`D`）。
- 传输：Steam 模式用 `ISteamNetworkingMessages`（走 Steam 中继），服务器模式用 `DATA` 转发。
- 开局时对手先用 `empty_kingdom.json`（只有城堡）占位。
- 同一房间每人国王必须不同（游戏按国王查找对手）。

踩过的坑：
- GC handle 在这个 IL2CPP 版本是指针（Il2CppGCHandle），不是 uint32。
- 往 `RunDefinition.Perks` / 国王特权配置里塞自建对象会让游戏崩溃或改坏存档 → 特权改为按各自进度。
- 玩家昵称在多人界面用英文字体，中文会显示成空格。
- 每年的进攻国王（`WaveProperties.King`）游戏是懒设置的，常为空。`RefreshMultiplayerMap`（对手布置）和 `PlayerBase.ReceivedLethalDamage`（对手城堡被打爆 → 扣命）都靠它找对手，为空就抛空引用：布置不刷新、城堡无敌。现在每帧把各年的 King 设成存活对手（轮换），见 `snapshot::assign_wave_kings`。
- 只调用 `RefreshMultiplayerMap` 重建对手地图时，对手的兵会出现在格子上，但不会被登记成敌军，玩家的兵会站着不动。必须走完整的 `SpawnWave`。同步局里游戏自己在年初调用的 `SpawnWave` 会被跳过（整局第一次除外，它负责初始化对手城堡），保证每年只生成一次。
- `MultiplayerGameSnapshots.Add` 遇到同一年已有布置时什么也不做，所以替换布置要直接改列表。
- 调试开关（放在 DLL 旁边）：`debug_autoroom.txt` 在房间里自动选国王并开局，`debug_autobattle.txt` 每年开局 8 秒后自动开战，`debug_counts.txt` 记录双方兵数和可攻击目标。
- 对手模式的一年里还有两处官方规则要关：`EndWave` 的“只剩一个对手 → 跳到第 33 年”，以及 `SubmitMultiplayerRun` 上传官方服务器。

## 组成
| 文件 | 作用 |
|---|---|
| `NineLink.exe` | 启动器（WPF）：找到游戏、核对构建号（`game.ver`）、启动并注入 |
| `ninelink_loader.exe` | 注入器：启动 9Kings.exe，等游戏窗口和 GameAssembly.dll 就绪后用 `LoadLibraryW` 注入 |
| `ninelink.dll` | mod 本体（C++）：通过 `il2cpp_*` 导出按类名/方法名访问游戏代码 |

构建：`powershell -File build.ps1`（需要 VS 2022 Build Tools C++ 工作负载 + .NET 8 SDK），输出到 `dist\`。
游戏运行时 `ninelink.dll` 被占用，重新构建前先关掉游戏。

## 阶段 1 结果（2026-11-23，构建号 26768399 / GameAssembly 0x6AFE071D）
- 注入成功，IL2CPP API 全部可用，Assembly-CSharp 2751 个类全部可读。
- 类型清单：`dist\log\dump_<GameAssembly时间戳>.cs`。

## 关键发现：游戏自带异步联机模式
`MultiplayerSystems` 命名空间里有官方的异步 PvP：
- `Multiplayer.GetMatchmaking()` → `MatchmakingResponse.Runs : MultiplayerRunData[]`（别的玩家录好的对局）
- `MultiplayerRunData.snapshot : MultiplayerGameSnapshots` → 每一年一份 `Gameplay.GameSnapshot`
  （国王、`Areas` 棋盘、政策、手牌、`RunDefinition` …），`GetWaveSnapshot(year)` 取出对应年份的棋盘
- `Multiplayer.SubmitRun(SubmitRunParams{snapshot,...})` 上传自己的对局
- `MultiplayerSession.GauntletEnemies` 是本局要打的对手列表
- 所有请求都是 JSON（Newtonsoft），地址来自 `MultiplayerRoutes.get_*Url()`

**联机方案**：不自己造对战，复用官方的快照对战。好友之间用 Steam P2P 交换 `GameSnapshot`，
在本地把 matchmaking 的结果换成好友的实时快照。**不向官方服务器提交任何 mod 数据**，避免影响官方排位。
