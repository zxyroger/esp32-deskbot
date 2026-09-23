# Scratch 离线版 + s3-extend 连接步骤

本机已安装的环境（可直接对照）：

| 组件 | 位置 |
| --- | --- |
| s3-extend 1.35 | `C:\Users\Administrator\AppData\Roaming\Python\Python313\site-packages\s3_extend` |
| 启动器 | `%APPDATA%\Python\Python313\Scripts\s32.exe`（还有 `esp32gw.exe`、`wsgw.exe`、`backplane.exe`） |
| Scratch 离线版 | Microsoft Store 版 Scratch Desktop 3.32.1 |

## 整条链路

```
Scratch 积木 ──WebSocket ws://127.0.0.1:9007──► wsgw (WebSocket 网关)
                                                   │  Banyan 主题
                                                   ▼
                                              esp32gw (ESP32 网关)
                                                   │  telemetrix (TCP)
                                                   ▼
                                          ESP32-S3 :31336  (本工程固件)
```

## 第 1 步：板子先上电并记下 IP

烧录后打开串口。板子的调试串口走**原生 USB 口**（USB Serial/JTAG），
插上 USB 线后用 `idf.py -p COM5 monitor` 看日志，会看到：

```
I (1234) wifi: 已连接 WiFi "xxx"
I (1234) wifi: 板子 IP 地址: 192.168.1.123   端口: 31336
```

如果一直连不上路由器，固件会自动开热点 `ESP32S3-Scratch`（可用 menuconfig 关闭），
这时让 PC 连上该热点，板子地址固定为 `192.168.4.1`。

## 第 2 步：PC 上启动 s3-extend 的 ESP32 服务器

```powershell
D:\esp\onegpio\tools\start_s3extend.ps1
```

> **本机必须这样启动**：Python 是微软商店版，`s3-extend` 的启动器装在
> `%APPDATA%\Python\Python313\Scripts`，这个目录**不在 PATH 里**。
> 直接敲 `s32` 会报 `FileNotFoundError`（因为 `s32` 内部要调用同目录的
> `backplane`、`wsgw`、`esp32gw`）。上面这个脚本会自动把该目录加进本次会话的
> PATH，再启动 `s32`。
>
> 想手动做的话就是这两行：
> ```powershell
> $env:Path = "$env:APPDATA\Python\Python313\Scripts;$env:Path"
> s32
> ```

看到类似输出即表示三个进程都起来了：

```
backplane started
Websocket Gateway started
ESP-32 Gateway started
```

想确认真的起来了，看端口是否被监听（`wsgw` 必须占住 9007）：

```powershell
Get-NetTCPConnection -State Listen | Where-Object LocalPort -in 9007,43124,43125
```

**停止**：在运行 `s32` 的那个窗口按 `Ctrl+C`，它会把三个子进程一起结束。
如果窗口已经关了、进程还赖着，可以：

```powershell
Get-Process s32,backplane,wsgw,esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force
```

> 想单独调某一个组件时：
> `backplane`、`wsgw -i 9007`、`esp32gw`（`esp32gw -m 主题名` 可改订阅主题），
> 同样需要先把 `%APPDATA%\Python\Python313\Scripts` 加到 PATH。

### 最省事：重启 PC 后点积木自动拉起服务（按需模式）

```powershell
D:\esp\onegpio\tools\install_autostart.ps1 -OnDemand
```

装完之后，重启 PC 什么都不用管：登录时只常驻一个十几 MB 的**微型启动器**
（`127.0.0.1:8000`），你在 Scratch 里**点任意积木**，它就替你跑
`start_s3extend.ps1 -Background`，约 15 秒后自动连上，排队的积木指令再补发。

```
点积木 → 0.7 秒还没连上 → POST /start → 启动器拉起三件套 → 自动重连 9007
```

* 扩展跑在编辑器沙箱里，**起不了本机进程**，所以必须有这么个本机小进程接住请求；
  它只用 Python 标准库，不占 9007 / 43124 / 43125。
* 服务本来就在跑时不会去打扰启动器；连点积木有 8 秒冷却。
* 新加了两块积木：**「启动本地服务」**（手动触发一次）和 **「本地服务状态」**
  （报告积木，能看出启动器在不在、服务连没连上）。
* 状态积木的文案会跟着变：
  `正在启动本地服务…（点积木触发的自动拉起，约 10~20 秒）` /
  `已连上本地服务，等点「连接板子 IP」` /
  `本地服务未启动，启动器也没在运行（先跑一次 tools\start_launcher.ps1）`。

日常用这几条命令：

```powershell
D:\esp\onegpio\tools\start_launcher.ps1 -Check   # 启动器 / 服务 / 自启 三合一状态
D:\esp\onegpio\tools\start_launcher.ps1          # 现在就想让启动器跑起来
D:\esp\onegpio\tools\start_launcher.ps1 -Stop    # 停启动器 (正在跑的服务不受影响)
```

浏览器打开 <http://127.0.0.1:8000/> 有状态页，带「启动服务 / 停止服务」按钮；
排错用 <http://127.0.0.1:8000/logs>。
想验证这条路真的通（会先停服务再靠点积木拉起来）：

```powershell
node D:\esp\onegpio\tools\test_autostart_live.js --restart
```

### 不想每次手动启动？装成开机自启（全量模式）

> 这一种是"登录后就把三件套全起起来"，跟上面的按需模式**互斥**
> （装哪个都会摘掉另一个的登录项）。按需模式请见上一节。

```powershell
D:\esp\onegpio\tools\install_autostart.ps1
```

登录 Windows 后会后台自动拉起（不弹窗），日志在 `%LOCALAPPDATA%\s3extend\logs\`。
配套命令：

```powershell
D:\esp\onegpio\tools\start_s3extend.ps1 -Check    # 看状态
D:\esp\onegpio\tools\stop_s3extend.ps1            # 停止
D:\esp\onegpio\tools\install_autostart.ps1 -Remove  # 取消自启
```

自启拉起的守护进程会盯着三个组件（每 1 秒巡检一次），任何一个掉线 1~2 秒内自动重启——
这样即使开机时板子还没上电（`esp32gw` 会因为连不上板子而退出），
等你把板子上电后再点一次 IP 积木就能连上。

另外，`esp32gw` 还有一种"进程还在、但已经不响应任何积木"的坏状态：
上游网关的 Banyan 接收循环没有异常保护，板子重启 / WiFi 掉线 / 重新烧录固件
带来的一个 `ConnectionResetError` 就能把它打死，而进程不会退出。
守护进程会检查它的错误日志，一旦发现未处理异常就自动重启它（约 1~2 秒），
日志里会写 `esp32gw 的接收循环已失效 ... 重启它`。
用 `start_s3extend.ps1 -Check` 可以直接看到 `esp32gw 健康检查: 正常 / 已失效`。

### 自动重连（网关重启后不用再点积木）

网关每次启动都要等 Scratch 发来第一条 `ip_address` 报文才会去连板子，所以
只要它被重启（看门狗修复、进程崩了、开机自启），照理说都得手动再点一次
「连接板子 IP」。守护进程把这一步也代劳了：

* 每次确认连上板子，就把这个 IP 记到 `%LOCALAPPDATA%\s3extend\logs\last_board_ip.txt`；
* 之后只要发现网关是**新实例**，3 秒后自动把记下的 IP 重发一次，
  日志里会写 `新网关实例 (PID x): 3 秒后自动重发板子 IP ...` 和 `已自动重发 ip_address ...`；
  发出去之后 4 秒还没连上就再补发一次（网关刚起来时可能还没订阅上主题，
  第一条会被 PUB/SUB 丢掉）；
* 连不上（比如板子还没上电）就一直补发，最多 6 次，之后停下等你手动点一次
  IP 积木（成功后会重新计数）。

> `esp32gw` 的启动逻辑还有一处上游 bug：它把启动后收到的**第一条**报文当成板子 IP。
> Scratch 里正在跑的积木指令会先到，于是 `transport_address` 一直是 `None`，
> telemetrix 抛 `RuntimeError: A TCP/IP address must be specified when using WI-FI.`，
> 网关当场退出——表现就是"点了 IP 积木要等几十秒，甚至要点好几次"。
> `tools\apply_local_patches.py` 的**补丁 3** 让它跳过所有非 `ip_address` 的报文，
> 打完补丁要重启网关（或直接重跑 `start_s3extend.ps1 -Background`）。

所以正常情况下：板子重启 / WiFi 掉线 / 网关被看门狗重启，几秒后连接会自己恢复，
Scratch 里的「连接状态」积木也会从"已断开"变回"已连接板子 <IP>"。
想关掉这个行为，用 `-DisableAutoReconnect` 参数启动守护进程即可。

### 板子 IP 自动发现（IP 积木其实可以不用填）

板子固件默认会每 2 秒向局域网广播一次自己的地址：

```
I (xxxx) wifi: UDP beacon started: broadcasting 192.168.0.103:31336 on port 31337 every 2s
```

PC 侧的守护进程会听这个广播（也可以主动发 `ONEGPIO?` 探测，毫秒级应答），
发现后自己把 IP 发给网关，因此：

* **启动时不用再去串口抄 IP**，只要板子和 PC 在同一个局域网；
* 板子换了 IP（DHCP 变化、换路由器）也能自己找回来；
* 守护进程每 15 秒会重试一次发现，所以"PC 先开机、板子后上电"也能自动连上；
* Scratch 扩展收到网关报上来的地址后会自动采用它，**不点「连接板子 IP」积木
  也能直接发指令**（留空即可）。

老固件没有广播也没关系：`tools\find_board.py` 会退回"扫本机 /24 网段 + 握手
校验固件版本"的老办法，只是慢几秒。想手动找一次板子：

```powershell
python D:\esp\onegpio\tools\find_board.py
```

另外，`IP 地址积木` 仍然保留：手动方式启动（`-Logs`）或者要指定另一块板子时，
填上 IP 就优先用它。

## 第 3 步：在 Scratch 里加载 ESP32 扩展

ESP32 扩展不在 Scratch 官方扩展库里，必须额外加载。**先说结论**：

> **原版 Scratch Desktop (3.32.1) 没有"通过 URL 加载扩展"的入口。**
> 我把它 `app.asar` 里的代码翻过了：那段 `prompt('Enter the URL of the extension')`
> 确实存在，但触发条件是"扩展库列表里出现一个没有 extensionId 的条目"，
> 而这个版本的内置扩展列表只有 12 个官方扩展
> (makeymakey / microbit / wedo2 / ev3 / boost / gdxfor / music / pen /
> text2speech / translate / videoSensing / faceSensing)，**没有任何一项能触发它**。
> 所以"把 URL 填进扩展库输入框"这条路，在原版桌面版上是走不通的。

下面三条路，按推荐顺序：

### 路线 A（推荐，完全离线）：TurboWarp + 本项目自带的扩展

本工程自带了一个等价的扩展脚本：[scratch/esp32s3.js](../scratch/esp32s3.js)，
它直接连 `ws://127.0.0.1:9007`，并且**使用正确的 Banyan 主题名**
`to_esp32_gateway`，绕开了上游仓库那处 `to_esp8232_gateway` 的拼写问题。

1. 安装 [TurboWarp Desktop](https://desktop.turbowarp.org/)（免费、可离线，兼容 Scratch 3 工程）；
2. 启动本地 HTTP 服务，把扩展挂到 `http://127.0.0.1:8000/esp32s3.js`：
   ```powershell
   D:\esp\onegpio\tools\start_launcher.ps1
   ```
   （`start_launcher.ps1` 的启动器也提供同一个地址，而且它还负责"点积木自动拉起
   服务"，所以推荐用它；只想单纯托管文件也可以用
   `D:\esp\onegpio\tools\serve_scratch_extension.ps1`。）
3. TurboWarp 里点左下角 **添加扩展**，在扩展库中选择加载**自定义扩展**，
   选 URL 方式并把 `http://127.0.0.1:8000/esp32s3.js` 填进去；
   也可以直接用"从文件加载"，选 `D:\esp\onegpio\scratch\esp32s3.js`，
   这样连本地 HTTP 服务都不用起。

加载后积木面板出现 **ESP32-S3 (s3-extend)**，共 11 块积木：
连接板子 IP、数字引脚设为 0/1、PWM 输出 %、舵机角度、数字读、模拟读、超声波(厘米)，
外加两块状态积木：**连接状态**（报告积木）和 **已连接板子?**（布尔积木），
以及两块服务积木：**启动本地服务**（命令积木，手动触发按需启动）
和 **本地服务状态**（报告积木，报告启动器/服务的连接情况）。

除此之外还有板载硬件的积木：**屏幕背光 / 屏幕颜色**（0x70 / 0x71）、
**播放音调 / 停止播放音频 / 麦克风检测 / 麦克风响度**（0x72~0x74）、
**朗读文字 / 停止朗读**（0x75 / 0x76），以及摄像头的一组：
**摄像头尺寸 / 摄像头质量 / 打开摄像头（画面显示在当前角色上）|
关闭摄像头 / 拍一张照片（变成新造型）/ 摄像头状态 / 照片（数据 URL）**。

> 摄像头那组是**流式播放**：点「打开摄像头」后板子连续出图，每一帧原地刷到当前
> 角色的「摄像头画面」造型上，舞台上就是实时画面；点「关闭摄像头」或编辑器的
> 停止按钮停流。实测 QVGA 约 7~10 帧/秒、VGA 约 3~4 帧/秒（想更流畅用 QVGA）。

> 摄像头那组要额外打 `tools\apply_local_patches.py` 的**补丁 9 / 10 / 11**
> （打完重启网关），否则会出现"能连上板子、`摄像头状态` 也回得来，但照片永远
> 收不到"。原理和排查过程见 [camera-ov2640.md](camera-ov2640.md) 和
> [camera-debug-notes.md](camera-debug-notes.md)。

### 路线 B（联网）：官方 OneGPIO 在线编辑器

打开 <https://mryslab.github.io/s3onegpio/>，点左下角"添加扩展"，
在 "Choose an Extension" 里选 **OneGpio ESP32**。
注意它用的是上游的主题名，如果积木没反应，见下面"常见问题"第 2 条。

### 路线 C（完全离线自建）：编译 s3onegpio

fork `MrYsLab/s3onegpio`（scratch-gui + scratch-vm 的定制版），
按其 `notes/rebuilding_and_rerunning.txt` 用 node 19.9.0 / yarn 1.22 构建，
得到本地可运行的 Scratch，扩展已内置。对只想拖积木的人来说成本偏高。

## 第 4 步：填 IP 地址

把第 1 步记下的 IP 填进 **IP 地址积木** 并执行一次。
扩展会把 `{"command":"ip_address","address":"192.168.1.123"}` 经 WebSocket →
Banyan → esp32gw，网关收到后才去连板子的 `31336` 端口；
连上时串口会打印 `客户端已连接: 192.168.1.x`。

## 怎么确认"连接板子 IP"执行成功了

### 0. 直接看「连接状态」积木（最省事，本项目扩展特有）

本项目自带的 [scratch/esp32s3.js](../scratch/esp32s3.js) 有两块状态积木：

| 积木 | 显示内容 |
| --- | --- |
| `连接状态`（报告） | `本地服务未连接（先运行 start_s3extend.ps1）` / `已连上本地服务，等点「连接板子 IP」` / `正在连接 192.168.0.107 …` / `已连接板子 192.168.0.107（固件 3.2.0）` / `板子连接已断开…，请再点一次「连接板子 IP」` / `连接失败：…` |
| `已连接板子?`（布尔） | 真/假，可以直接放进 `如果 … 那么` |

把 `连接状态` 显示成监视器（积木面板里右键积木 → 显示监视器），就能一直看到当前状态：

* 点「连接板子 IP」的瞬间会变成 `正在连接 …`，这一步是扩展本地立刻给出的；
* 后台守护进程在网关日志里看到 `Successfully connected to:` 就会报 `已连接板子 <IP>（固件 x.y.z）`；
* 板子掉线、网关被自动重启、连不上板子（错误行会一并显示）都会在 5~10 秒内反映出来。

> 状态回传依赖后台守护进程（`-Background` / 开机自启模式）。用 `-Logs` 或裸跑 `s32`
> 时没有守护进程，就只有前面两档状态（本地服务是否连上、正在连接）——
> 排查问题建议直接用后台模式，配 `start_s3extend.ps1 -Check` 看网关健康。

另外，「连接板子 IP」这块积木本身依旧不会自己弹提示（上游设计如此），
所以除了看状态积木，还可以看下面这些外部证据（按可靠性从高到低）：

### 1. 板子串口日志（最权威）

执行积木的瞬间，串口应该多出一行：

```
I (12345) tmx_server: client connected: 192.168.0.103:52341
```

括号里的 IP 是**你 PC 的地址**、后面是 PC 的随机端口。
**看到这一行 = TCP 已经连上，IP 填对了**；断开时会打印
`client disconnected, waiting for the next connection`。
如果 IP 填错或不在同一网段，这一行永远不会出现。

### 2. PC 端网关日志（第二可靠）

用带日志的模式启动 s3-extend：

```powershell
D:\esp\onegpio\tools\start_s3extend.ps1 -Logs
```

执行积木后应该看到（这是实测过的输出）：

```
[esp32gw] TelemetrixAioEsp32 Version: 2.1.1
[esp32gw] Successfully connected to: 192.168.1.123:31336
[esp32gw] Telemetrix4Esp32WIFI Firmware Version: 3.2.0
```

`Successfully connected to:` 出现 = 网关连上板子并读到了固件版本 3.2.0。
（默认的 `start_s3extend.ps1` 用的是 s32 启动器，它把子进程输出藏起来了，
**看不到这些日志**——所以排查连接问题请用 `-Logs`。）

### 3. 连接失败时是什么样子

* `-Logs` 模式：会打印 `[esp32gw] Can't open connection to 192.168.x.x`，
  然后网关退出。
* 默认（s32）模式：窗口里会出现 `ESP-8266 Gateway exited.`
  （上游文案写错了，其实指的是 ESP32 网关），随后**整个栈被关掉**。
  这时改对 IP，重新运行 `start_s3extend.ps1` 即可。

### 4. 功能验证（最终确认）

拖一个「数字引脚 [4] 的值」或「模拟引脚 [32] 的值」积木，配合
`重复执行` + `说` 显示出来：

* 数值会变化 → 上行（命令）和下行（上报）都通了；
* 一直是 0 → 连接可能成功但上报没回来，先检查引脚（见 `pins-esp32s3.md`）。

### 不经过 Scratch 的快速排查

如果上面都拿不准，先用项目自带的脚本直接验板子和网络：

```powershell
python D:\esp\onegpio\tools\pc_tcp_check.py 192.168.1.123
```

能打印固件版本、模拟输入数值，说明固件和网络没问题，
那么问题就只可能在 s3-extend 或 Scratch 扩展这一层。

## 常见问题排查

**1. 板子连不上 / 串口没有 "客户端已连接"**

* 先在 PC 上跑 `python tools\pc_tcp_check.py <板子IP>` 验证固件与网络；
* 确认 PC 与板子在同一个网段（板子 AP 模式下 PC 要连 `ESP32S3-Scratch`）；
* 确认端口一致：固件 `CONFIG_TMX_TCP_PORT` = 客户端 `ip_port` = 31336。

**2. 扩展积木没反应（网关没有任何日志）**

Scratch 扩展与网关之间是通过 **Banyan 主题名** 对接的，两边必须完全一致：

* 本机安装的 `esp32gw`：订阅主题 `to_esp32_gateway`，上报主题 `from_esp32_gateway`
* 上游 `s3onegpio` 仓库 master 里 ESP32 扩展的 `connect()` 写的是
  `{"id": "to_esp8232_gateway"}`（**多了一个 2**），两边对不上

**用本项目自带的 `scratch/esp32s3.js` 不会遇到这个问题**：它用的是正确主题名，
而且已经端到端实测过（模拟读能回传数值、数字读能收到 0/1 变化）。

检查方法：看浏览器/桌面版的网络面板里 WebSocket 发出的第一条消息。

两种修法：

* 若扩展使用的是正确主题 `to_esp32_gateway`：`s32` 直接可用，无需改动。
* 若扩展使用的是 `to_esp8232_gateway`：
  1. 编辑 `%APPDATA%\Python\Python313\site-packages\s3_extend\gateways\esp32_gateway.py`，
     把 4 处 `'from_esp32_gateway'` 改成 `'from_esp8232_gateway'`；
  2. 按 `esp32gw -m to_esp8232_gateway` 启动网关（`wsgw` 仍用 `-i 9007`）。

**3. IP 积木执行后 Scratch 弹 "Web Socket Closed"**

说明 `wsgw`（9007 端口）没在跑或已经退出。重新执行 `s32`，并确认 9007 没被占用：

```powershell
Get-NetTCPConnection -LocalPort 9007 -ErrorAction SilentlyContinue
```

**4. 某些引脚点了没反应**

见 [pins-esp32s3.md](pins-esp32s3.md)：22/23/25 在 ESP32-S3 上不存在，
26~37 默认被固件拒绝（Flash/PSRAM）。

**5. 输入值一直是 1（数字读）/ 一直是 0（模拟读）**

* 数字输入建议改用"上拉输入"接法（按键一端接地），固件会在设置模式后立刻上报真实电平；
* 模拟输入要接到 `GPIO1 ~ GPIO6`（Scratch 里选 32~39 即可，固件自动映射）。

## 不经过 Scratch 的两种验证方式

```powershell
# 1) 纯 socket，直接测协议
python tools\pc_tcp_check.py 192.168.1.123

# 2) 用 s3-extend 同款 Python 库跑一遍数字输出/模拟输入
python tools\pc_telemetrix_demo.py 192.168.1.123
```

两个脚本都能通，说明"固件 + 网络 + PC 端库"这条链路完全正常，
剩下如果 Scratch 还是不动，问题一定在扩展加载或主题名（第 2 条）。

## 屏幕积木（背光 / 颜色）

扩展里有两块管板载 ILI9342C 屏幕的积木：

* **屏幕背光 [开/关]**：`开` 发送亮度 100，`关` 发送 0（固件协议本身支持
  1~100 的亮度值，以后要加"亮度 %"积木不用改固件）；
* **屏幕颜色设为 [#rrggbb]**：整屏填这个颜色，Scratch 的颜色选择器给的就是
  `#rrggbb`，扩展会转成 R/G/B 三个字节发给板子。

这两块走的是自定义 Telemetrix 命令（0x70 / 0x71），所以网关侧需要
`python tools\apply_local_patches.py` 打上**补丁 6**，并且打完要重启网关：

```powershell
python tools\apply_local_patches.py
Get-Process esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force   # 守护进程会自动拉起
```

如果升级过 s3-extend（pip），补丁会丢，重跑一次上面的命令即可。
只有屏幕积木没反应、其它积木正常，基本就是这个原因。

不想用 Scratch 验证这两条命令时，可以直接往本地 backplane 发一条报文：

```python
# python tools\... 里的 banyan_publish.py 需要 msgpack 格式的 payload，
# 简单起见用 python -c 直接发 (需要装 zmq/msgpack, s3-extend 自带)
import zmq, msgpack, time
ctx = zmq.Context(); pub = ctx.socket(zmq.PUB)
pub.connect("tcp://127.0.0.1:43124"); time.sleep(0.6)
pub.send_multipart([b"to_esp32_gateway",
                    msgpack.packb({"command": "lcd_color", "red": 255,
                                   "green": 0, "blue": 0}, use_bin_type=True)])
time.sleep(0.3); pub.close(0); ctx.term()
```

板子串口上会打出 `tmx_core: lcd color: #ff0000` / `lcd backlight: off` 之类的日志。

## 音频积木（音调 / 麦克风）

扩展里还有四块板载 ES8311 音频的积木（细节见 [audio-es8311.md](audio-es8311.md)）：

* **播放音调 频率 `440` Hz 时长 `500` 毫秒 音量 `60` %**：发给板子 0x72，
  固件实时合成正弦波，经 DAC → PA → 喇叭放出来（非阻塞，别的积木照跑）；
* **停止播放音频**：0x73，立刻停音；
* **麦克风检测 `开` / `关`**：0x74，打开后板子才开始按变化上报响度；
* **麦克风响度**：报告积木，读的是板子上报的 0~100（要先打开「麦克风检测 开」）。

这些同样走自定义命令（0x72 ~ 0x74 + 上报 0x0D），所以网关侧需要
`python tools\apply_local_patches.py` 打上**补丁 7**（和屏幕积木一样，
打完要重启网关，见上一节）。只有音频积木没反应、其它正常，就是这个原因。

不经过 Scratch 验证硬件（先 `tools\stop_s3extend.ps1` 让出板子）：

```powershell
python tools\pc_tcp_check.py <板子IP> --tone 1000 --tone-ms 800   # 应该听到 1kHz 响一声
python tools\pc_tcp_check.py <板子IP> --mic 5                     # 对着麦克风说话, 看响度
```

没声音 / 响度一直是 0 的排查见 [audio-es8311.md](audio-es8311.md) 的排障表。

## 朗读文字积木（板载中文语音合成）

扩展里还有两块让板子"说话"的积木（细节见 [tts-esp-tts.md](tts-esp-tts.md)）：

* **朗读文字 [文本]**：文字经网关按 UTF-8 拆包（0x75）发给板子，板子用
  esp-tts **本地**合成中文语音，从 ES8311 喇叭念出来；
* **停止朗读**：0x76，立刻停。

它们是自定义命令，所以网关侧需要 `python tools\apply_local_patches.py` 的
**补丁 8**（打完要重启网关，见前面"屏幕积木"那节的命令）。

不用 Scratch 验证板子念得对不对（先 `tools\stop_s3extend.ps1`）：

```powershell
python tools\pc_tts_check.py <板子IP> "你好，我是小乐" --play
```

它会让板子念这段文字，并把板子合成的 PCM 回传存成 wav（`--play` 顺带用 PC 喇叭放一遍），
所以能直接确认"念出来的是不是这几个字"。命令行传中文有编码问题时用
`--hex-utf8 e4bda0e5a5bd`。
