# 服务安装说明（登录自启 + 点积木自动拉起）

这份文档是**照着敲就能装完**的操作步骤，目标只有一个：

> 重启 PC 之后什么都不用管，打开 Scratch 点任意积木，s3-extend 服务自己起来。

全程不需要管理员权限（只有想用"计划任务"方式时才需要）。

---

## 0. 先搞清楚要装什么

这套"服务"由 5 个进程组成，但**登录时只需要常驻其中的小启动器**：

| 组件 | 作用 | 端口 |
| --- | --- | --- |
| 微型启动器 `onegpio_launcher.py` | 接住 Scratch 发来的"启动服务"请求 | `127.0.0.1:8000` |
| 守护进程 `supervise_s3extend.ps1` | 盯着下面三个组件，掉线自动重启 | — |
| `backplane` | Banyan 消息总线 | `43124` / `43125` |
| `wsgw` | 给 Scratch 用的 WebSocket 网关 | `9007` |
| `esp32gw` | 连板子的 TCP 网关 | 出方向连板子 `31336` |

两种登录自启模式**只能选一种**（装一种会自动摘掉另一种的登录项）：

| | **按需模式（推荐）** | 全量模式 |
| --- | --- | --- |
| 登录后起什么 | 只起微型启动器（十几 MB） | 直接起三件套 |
| 三件套什么时候起 | 你在 Scratch 里点积木时 | 登录的时候 |
| 安装命令 | `install_autostart.ps1 -OnDemand` | `install_autostart.ps1` |

下面按**按需模式**走。想改成全量模式，看第 6 步。

---

## 第 0 步：确认前置条件

在 PowerShell 里逐条执行，三条都通过再往下走。

```powershell
# 1) Python 在不在（本机是 3.13.x）
python -V

# 2) s3-extend 装没装（本机是 1.35）
python -m pip show s3-extend

# 3) 三个后端程序能不能找到
D:\esp\onegpio\tools\start_s3extend.ps1 -Check
```

第 3 条应该打印 `环境检查通过 (backplane / wsgw / esp32gw 都能找到)。`

**没过怎么办：**

```powershell
# 没装 s3-extend
python -m pip install --user s3-extend

# 装完 s3-extend，给第三方包打本地补丁（幂等，pip 升级后可以再跑一次）
python D:\esp\onegpio\tools\apply_local_patches.py
```

打补丁这一步别跳过：不打的话会出现"点了 IP 积木要等几十秒才连上""换个网络就得重启整套服务"这类怪问题。

---

## 第 1 步：先手动跑一次，确认服务本身没问题

先把服务前台拉起来，看到三行 started 就说明服务端 OK，这一步和自启无关。

```powershell
D:\esp\onegpio\tools\start_s3extend.ps1
```

期望输出：

```
backplane started
Websocket Gateway started
ESP-32 Gateway started
```

再开一个窗口确认端口：

```powershell
Get-NetTCPConnection -State Listen | Where-Object LocalPort -in 9007,43124,43125 |
    Select-Object LocalAddress,LocalPort
```

看到 `9007` 就说明 Scratch 要连的那个网关已经就位。确认完把它停掉，回到干净状态：

```powershell
# 在前台那个窗口按 Ctrl+C，然后执行兜底清理
D:\esp\onegpio\tools\stop_s3extend.ps1
```

---

## 第 2 步：安装登录自启（按需模式）

**用普通窗口运行即可**，不要用管理员窗口：

```powershell
D:\esp\onegpio\tools\install_autostart.ps1 -OnDemand
```

脚本会按顺序尝试两种落地方式，**能成哪种算哪种，两种都算安装成功**：

1. 先试"计划任务"（需要管理员）——普通窗口会失败，这是**预期行为**；
2. 失败就自动改用"启动文件夹"里的 `onegpio_launcher_autostart.vbs`（不需要管理员），用 `wscript` 隐藏窗口启动，不闪黑框。

所以如果你在输出里看到这一行，**不要当成报错**：

```
注册计划任务失败 (通常是没有以管理员身份运行), 改用启动文件夹方式。
已安装启动项: C:\Users\<用户名>\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\onegpio_launcher_autostart.vbs
```

> 关于 `Register-ScheduledTask : 拒绝访问。HRESULT 0x80070005`：
> 计划任务的根目录只允许管理员写入，而普通窗口是 UAC 过滤后的标准令牌
> （`Mandatory Label\Medium Mandatory Level`，Administrators 组是 deny only），
> 所以注册必然被拒。**不是脚本 bug，也不是任务名冲突**，回退到启动文件夹即可，
> 效果和计划任务基本一样（差别只是不出现在"任务计划程序"里，也没有失败自动重启计数）。

装完立刻确认一下状态：

```powershell
D:\esp\onegpio\tools\install_autostart.ps1 -Status
```

只要看到 `[启动文件夹] 按需模式 ...: ...onegpio_launcher_autostart.vbs`，第 2 步就完成了。

### 可选：确实想用计划任务

想统一在"任务计划程序"里管理，或者要那份"失败自动重启 3 次"的看护，就用管理员权限重装一次：

```powershell
# 方式 A：已经开好管理员 PowerShell 的话
D:\esp\onegpio\tools\install_autostart.ps1 -OnDemand -Mode Task

# 方式 B：从普通窗口弹一次 UAC（需要你在桌面上点"是"）
Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','D:\esp\onegpio\tools\install_autostart.ps1','-OnDemand','-Mode','Task'
```

装成计划任务后，脚本会**自动删掉**启动文件夹里的那个 vbs（两种方式互斥，防止双份常驻），`-Status` 里会变成 `[计划任务] 按需模式 ...`。

---

## 第 3 步：不重启就让它先跑起来，并验证

```powershell
D:\esp\onegpio\tools\start_launcher.ps1
D:\esp\onegpio\tools\start_launcher.ps1 -Check
```

`-Check` 的期望输出（数值会不同）：

```
---- 启动器 ----
  运行中: http://127.0.0.1:8000/  (扩展 URL /esp32s3.js)
  PID 8040, 已运行 293.2 秒, 累计拉起服务 0 次
---- s3-extend 服务 ----
  未运行: 点任意积木时, 扩展会请启动器把它拉起来。
---- 登录自启 ----
[启动文件夹] 按需模式 (登录后只起微型启动器): ...\Startup\onegpio_launcher_autostart.vbs
日志目录: C:\Users\<用户名>\AppData\Local\s3extend\logs
```

三块的状态含义：

* **启动器"运行中"** → 自启要装的东西已经活了；
* **服务"未运行"** → 正常，按需模式就是这样，等你点积木；
* **登录自启有 `[启动文件夹]` 或 `[计划任务]`** → 重启后能自动起来。

### 趁热跑一次端到端（可选，但强烈建议）

它会先停掉服务，再模拟"点积木"把它拉起来，确实是全链路验证：

```powershell
node D:\esp\onegpio\tools\test_autostart_live.js --restart
```

期望看到 `✓ 服务被积木自动拉起 (大约 15s)` 和 `✓ 板子也连上了`。

浏览器也可以直接打开 <http://127.0.0.1:8000/>，页面上有「启动服务 / 停止服务」按钮和实时状态。

---

## 第 4 步：重启 PC 后验证

1. 正常重启，**不要手动运行任何脚本**；
2. 登录后等十几秒，开一个 PowerShell：

   ```powershell
   D:\esp\onegpio\tools\start_launcher.ps1 -Check
   ```

   应该显示启动器"运行中"，服务"未运行"；
3. 打开 TurboWarp，扩展 URL 填 `http://127.0.0.1:8000/esp32s3.js`，或用你原来加载扩展的方式；
4. 点任意一块积木（数字输出、读引脚、连接板子 IP 都行），等 10~20 秒；
5. 状态积木显示连上，或 `-Check` 显示 `9007 已监听` —— 全链路通了。

> 注意：这是**登录自启**，不是开机自启。s3-extend 装在当前用户的 `%APPDATA%` 下，
> 必须在"你登录之后"以你的身份运行才能找得到那些命令，这也是脚本用 `-AtLogOn`
> 而不是开机触发的原因。

---

## 第 5 步：日常命令速查

| 想干什么 | 命令 |
| --- | --- |
| 三合一状态（启动器 / 服务 / 自启） | `tools\start_launcher.ps1 -Check` |
| 现在就把启动器跑起来 | `tools\start_launcher.ps1` |
| 放行自己的编辑器来源 | `tools\start_launcher.ps1 -Restart -AllowOrigin https://my.editor` |
| 停启动器（不影响正在跑的服务） | `tools\start_launcher.ps1 -Stop` |
| 重启启动器 | `tools\start_launcher.ps1 -Restart` |
| 前台跑启动器看日志 | `tools\start_launcher.ps1 -Foreground` |
| 停三件套（启动器留着，点积木还能再拉起来） | `tools\stop_s3extend.ps1` |
| 手动起三件套（前台看日志） | `tools\start_s3extend.ps1` |
| 只看环境 + 端口，不启动 | `tools\start_s3extend.ps1 -Check` |
| 看自启装没装 | `tools\install_autostart.ps1 -Status` |
| 状态页 | <http://127.0.0.1:8000/> |
| 启动器日志接口 | <http://127.0.0.1:8000/logs> |

---

## 第 6 步：切换模式 / 卸载

```powershell
# 切到"全量模式"：登录直接把三件套全起起来（会自动摘掉按需模式的登录项）
D:\esp\onegpio\tools\install_autostart.ps1

# 切回"按需模式"
D:\esp\onegpio\tools\install_autostart.ps1 -OnDemand

# 卸载自启（计划任务和启动文件夹的登录项一起清掉）
D:\esp\onegpio\tools\install_autostart.ps1 -Remove
```

卸载只动登录项，**不会**停掉正在跑的进程；要停进程用 `stop_s3extend.ps1` 和 `start_launcher.ps1 -Stop`。

---

## 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `Register-ScheduledTask : 拒绝访问。HRESULT 0x80070005` | 普通窗口注册不了计划任务，属正常。脚本会自动回退到启动文件夹；想用计划任务见第 2 步的"可选"。 |
| `找不到 s32，说明 s3-extend 还没装好` | `python -m pip install --user s3-extend`，然后重跑 `start_s3extend.ps1 -Check`。 |
| 直接敲 `s32` 报 `FileNotFoundError: 'backplane'` | 本机 Python 装的是 `%APPDATA%\Python\Python313\Scripts`，不在 PATH 里。别直接敲 `s32`，用 `start_s3extend.ps1`（它会临时补 PATH）。 |
| `-Check` 说启动器没运行 | 跑一次 `start_launcher.ps1`；如果报端口被占，换 `-Port 8001`。 |
| 点了积木但 `9007` 一直不监听 | 先看 <http://127.0.0.1:8000/logs> 里启动器的报错，再跑 `start_s3extend.ps1 -Logs` 前台看三个组件的日志。 |
| `launcher.log` 里 `POST /start` 是 **403** | 扩展所在页面的来源没被启动器放行（这是防"任意网页偷开你本机服务"的校验）。默认已放行 `turbowarp.org`、`scratch.mit.edu`、`penguinmod.com`、`adacraft.org`、本机来源，以及 TurboWarp 桌面版的 `tw-editor://`；用别的编辑器就 `start_launcher.ps1 -Restart -AllowOrigin https://你的编辑器` 重启动器。日志里会写明被拒的 `Origin=`。 |
| 状态积木提示"启动器拒绝了自动拉起（HTTP 403…）" | 同上，说明启动器活着但拦了来源；按上一条放行后重新点积木即可。 |
| 状态积木提示"本地服务未启动，启动器也没在运行" | 登录自启没生效或启动器被停了，`start_launcher.ps1` 手动起一次，再 `install_autostart.ps1 -Status` 查登录项。 |
| 重启后没自动起来 | 查 `install_autostart.ps1 -Status`：两个模式都没看到就是没装成功；确认 Startup 目录里有没有 `onegpio_launcher_autostart.vbs`。另外它只在**登录后**触发，只开机不登录不会起。 |
| 换了网络 / PC 的 IP 变了 | 不用重启服务，补丁 5 已把内部通信改成 `127.0.0.1` + `0.0.0.0`。若还是不通，重跑 `python tools\apply_local_patches.py` 并重启网关。 |
| 板子换了 IP | 不用手填，固件每 2 秒 UDP 广播，`find_board.py` 和守护进程会自动发现。想手动查：`python tools\find_board.py`。 |

---

## 附录：文件和日志都在哪

| 东西 | 路径 |
| --- | --- |
| 所有脚本 | `D:\esp\onegpio\tools\` |
| 安装/卸载自启 | `D:\esp\onegpio\tools\install_autostart.ps1` |
| 启动/查看启动器 | `D:\esp\onegpio\tools\start_launcher.ps1` |
| 启动/停止三件套 | `D:\esp\onegpio\tools\start_s3extend.ps1` / `stop_s3extend.ps1` |
| 守护进程 | `D:\esp\onegpio\tools\supervise_s3extend.ps1` |
| 登录项（启动文件夹方案） | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\onegpio_launcher_autostart.vbs` |
| 登录项（计划任务方案） | 任务计划程序里的 `onegpio launcher (on demand)` / `s3-extend ESP32 server` |
| 启动器 PID 文件 | `%LOCALAPPDATA%\s3extend\launcher.pid` |
| 日志目录 | `%LOCALAPPDATA%\s3extend\logs\` |
| 日志文件 | `supervisor.log`、`launcher.log`、`launcher.err.log`、`backplane.log`、`wsgw.log`、`esp32gw.log`（各配一份 `.err.log`） |
| 扩展脚本（TurboWarp 可直接填这个 URL） | `http://127.0.0.1:8000/esp32s3.js` |

> 早期文档/截图里出现过 `tools\install\_autostart.ps1`、`tools\start\_launcher.ps1`
> 这类路径，那是重构前的旧位置，**现在已经不存在**，一律以 `tools\` 下的平铺脚本为准。
