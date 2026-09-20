<#
.SYNOPSIS
    s3-extend 守护进程: 保证 backplane / wsgw / esp32gw 一直活着。

.DESCRIPTION
    esp32gw 有一个特性: 当它拿着 IP 去连板子却连不上时, 它会直接退出。
    开机自启时板子往往还没上电, 结果就是"服务起来了但网关没了"。

    这个脚本每 1 秒检查一次:
      * backplane 没在监听 43124 -> 启动它
      * wsgw 进程不在            -> 启动它 (监听 9007)
      * esp32gw 进程不在         -> 启动它
      * esp32gw 进程在但 stderr 里出现了堆栈 -> 说明它的 Banyan 接收循环
        已经死掉 (进程还活着, 但再也不响应任何积木指令) -> 重启它
      * 顺便把"网关/板子连接状态"回传给 Scratch 的「连接状态」积木
    日志写到 -LogDir (默认 %LOCALAPPDATA%\s3extend\logs)。

    正常使用不需要手工运行, 由 start_s3extend.ps1 -Background 拉起,
    由 stop_s3extend.ps1 停止。
#>
[CmdletBinding()]
param(
    [string]$LogDir = (Join-Path $env:LOCALAPPDATA 's3extend\logs'),
    [int]$IntervalSeconds = 1,
    [switch]$DisableAutoReconnect
)

$ErrorActionPreference = 'SilentlyContinue'
$env:PYTHONUNBUFFERED = '1'

$scriptDir = (Get-Command s32 -ErrorAction SilentlyContinue).Source
if (-not $scriptDir) {
    $scriptDir = Join-Path $env:APPDATA 'Python\Python313\Scripts\s32.exe'
}
$scriptDir = Split-Path -Parent $scriptDir
$env:Path = "$scriptDir;$env:Path"

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$supervisorLog = Join-Path $LogDir 'supervisor.log'
$pidFile = Join-Path $LogDir 'supervisor.pid'

# 记录自己的 PID, 供 start/stop 脚本精确识别 (避免按命令行误伤别的 powershell)
Set-Content -LiteralPath $pidFile -Value $PID -Encoding ASCII

function Write-Log {
    param([string]$Message)
    $line = "{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message
    Add-Content -LiteralPath $supervisorLog -Value $line
}

function Test-Port {
    param([int]$Port)
    return [bool](Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
}

# ---- backplane 绑定地址检查 --------------------------------------------------
# 上游的 backplane 会把"启动那一刻的本机 IP"绑死 (打完补丁 5 之后改成绑 0.0.0.0)。
# PC 换网络 / DHCP 换地址之后, 老 backplane 监听的地址就不再属于本机 (那个地址
# 甚至可能已经被板子等别的设备占用), 可 Test-Port 只看端口号, 会一直认为它健康。
# 这种"僵尸 backplane"谁连都连不上, 只能靠查监听地址才能发现。
function Test-BackplaneBind {
    param([int]$Port = 43124)
    $listens = @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
    if ($listens.Count -eq 0) { return $false }
    $localIps = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty IPAddress)
    foreach ($item in $listens) {
        if ($item.LocalAddress -eq '0.0.0.0' -or $item.LocalAddress -eq '127.0.0.1') { return $true }
        if ($localIps -contains $item.LocalAddress) { return $true }
    }
    return $false
}

# ---- 组件健康检查 ----------------------------------------------------------
# 背景: 上游的网关 (esp32gw / wsgw) 在它的 Banyan 接收循环里没有任何异常保护。
# 板子重启、WiFi 掉线、固件重新烧录都会让 PC 侧 socket 收到 ConnectionResetError,
# 这个异常会把整个接收协程打死。表现是: 进程还在, 但从此不处理任何消息,
# 在 Scratch 里就是"点什么积木都没反应"。进程不会退出, 所以只看进程在不在
# 是发现不了这种故障的 —— 只能靠 stderr 里有没有未处理异常来判断。
$errorLogBaseline = @{}

function Get-ErrorLogPath {
    param([string]$Name)
    return (Join-Path $LogDir "$Name.err.log")
}

function Get-ErrorLogSize {
    param([string]$Name)
    $path = Get-ErrorLogPath $Name
    if (Test-Path -LiteralPath $path) {
        return (Get-Item -LiteralPath $path).Length
    }
    return 0
}

# ---- 板子链路健康检查 ------------------------------------------------------
# esp32gw.log 里那行 "Successfully connected to: ..." 是**历史记录**: 板子掉线、
# WiFi 抖动、半死连接都会让 TCP 断掉, 但日志还留着那句话, 于是守护进程会一直
# 以为"已连接", 而 Scratch 点什么都没反应。这里查一下真实存在的 TCP 连接。
function Test-BoardLinkAlive {
    param([string]$Address, [int]$Port = 31336)
    if (-not $Address) { return $true }
    try {
        $conn = Get-NetTCPConnection -State Established -RemotePort $Port `
            -ErrorAction SilentlyContinue |
            Where-Object { $_.RemoteAddress -eq $Address }
        return [bool]$conn
    } catch {
        return $true    # 查不到就不误杀
    }
}

function Stop-Component {
    param([string]$Name)
    # 结束启动器 exe 及其 python 子进程 (子进程才是真正干活的)
    Get-Process -Name $Name -ErrorAction SilentlyContinue | ForEach-Object {
        $parentId = $_.Id
        Get-CimInstance Win32_Process -Filter "ParentProcessId=$parentId" -ErrorAction SilentlyContinue |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Stop-Process -Id $parentId -Force -ErrorAction SilentlyContinue
    }
    # 兜底: 启动器已退出、但 python 子进程还赖着的孤儿进程
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*$Name.exe*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Start-Component {
    param([string]$Name, [string[]]$Arguments)
    $exe = Join-Path $scriptDir "$Name.exe"
    $startParams = @{
        FilePath               = $exe
        WindowStyle            = 'Hidden'
        RedirectStandardOutput = (Join-Path $LogDir "$Name.log")
        RedirectStandardError  = (Join-Path $LogDir "$Name.err.log")
        ErrorAction            = 'Stop'
    }
    # 注意: 不能传空数组给 -ArgumentList, 否则参数验证会失败
    if ($Arguments -and $Arguments.Count -gt 0) {
        $startParams['ArgumentList'] = $Arguments
    }
    try {
        Start-Process @startParams | Out-Null
        # Start-Process 重定向会清空日志文件, 基线归零
        $errorLogBaseline[$Name] = 0
        Write-Log "已启动 $Name"
    } catch {
        Write-Log "启动 $Name 失败: $($_.Exception.Message)"
    }
}

function Test-Esp32GatewayHealth {
    # 记录上次连上的板子 IP (重启日志前先读, 方便排查时对照)
    $lastBoardIp = ''
    $gwLog = Join-Path $LogDir 'esp32gw.log'
    if (Test-Path -LiteralPath $gwLog) {
        $hit = Select-String -LiteralPath $gwLog -Pattern 'Successfully connected to:\s*(\S+)' -ErrorAction SilentlyContinue |
            Select-Object -Last 1
        if ($hit) { $lastBoardIp = $hit.Matches[0].Groups[1].Value }
    }

    Write-Log "esp32gw 的接收循环已失效 (stderr 出现未处理异常), 重启它"
    if ($lastBoardIp) {
        Write-Log "  上次连上的板子是 $lastBoardIp"
    }
    Publish-GatewayStatus -State 'disconnected' -Address $lastBoardIp `
        -Message '板子连接断开, 网关已自动重启'
    Stop-Component -Name 'esp32gw'
    # 等进程真的退出, 避免新旧实例同时订阅同一个主题
    for ($i = 0; $i -lt 10; $i++) {
        if (-not (Get-Process esp32gw -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 300
    }
    Start-Component -Name 'esp32gw'
    Write-Log "  重启完成: 请在 Scratch 里再点一次「连接板子 IP」积木"
}

# ---- 网关状态上报 (给 Scratch 的「连接状态」积木) ---------------------------
# 守护进程是唯一知道网关完整生命周期的地方, 所以由它上报:
#   waiting      网关在跑, 但还没连上板子 (等 Scratch 里的 IP 积木)
#   connected    已连上板子 (带板子 IP 和固件版本, 从网关日志里解析)
#   disconnected 板子连接断开, 网关已被重启
#   error        连板子失败 (附上网关 stderr 的最后一行)
$script:publishScript = Join-Path $PSScriptRoot 'banyan_publish.py'
$script:lastStatusKey = ''
$script:lastStatus = @{}
$script:heartbeatCount = 0
$script:publishUnavailableLogged = $false

function Get-PythonExe {
    $candidates = @()
    $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    $candidates += (Join-Path $env:ProgramFiles 'Python313\python.exe')
    $candidates += (Join-Path $env:ProgramFiles 'Python312\python.exe')
    $candidates += (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python313\python.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    return $null
}

$script:pythonExe = Get-PythonExe

function Publish-GatewayStatus {
    param(
        [Parameter(Mandatory = $true)][string]$State,
        [string]$Address = '',
        [string]$Firmware = '',
        [string]$Message = '',
        [switch]$Force
    )
    if (-not $script:pythonExe -or -not (Test-Path -LiteralPath $script:publishScript)) {
        if (-not $script:publishUnavailableLogged) {
            Write-Log "跳过状态上报: 找不到 python 或 banyan_publish.py"
            $script:publishUnavailableLogged = $true
        }
        return
    }

    $key = "$State|$Address|$Firmware|$Message"
    if (-not $Force -and $key -eq $script:lastStatusKey) { return }

    $script:lastStatus = @{ State = $State; Address = $Address; Firmware = $Firmware; Message = $Message }
    $argv = @($script:publishScript, '--state', $State)
    if ($Address) { $argv += @('--address', $Address) }
    if ($Firmware) { $argv += @('--firmware', $Firmware) }
    if ($Message) { $argv += @('--message', $Message) }
    try {
        & $script:pythonExe @argv 2>&1 | Out-Null
        $script:lastStatusKey = $key
        Write-Log "已上报网关状态: $key"
    } catch {
        Write-Log "上报网关状态失败: $($_.Exception.Message)"
    }
}

function Get-GatewayLogFacts {
    # 从 esp32gw.log 里解析最近一次连上的板子 IP 和固件版本
    $facts = @{ Address = ''; Firmware = '' }
    $gwLog = Join-Path $LogDir 'esp32gw.log'
    if (-not (Test-Path -LiteralPath $gwLog)) { return $facts }
    $text = ''
    try { $text = Get-Content -LiteralPath $gwLog -Raw -ErrorAction Stop } catch { return $facts }
    # 日志里是 192.168.0.107:31336, 只取 IP 部分
    $m = [regex]::Matches($text, 'Successfully connected to:\s*([^:\s]+):\d+')
    if ($m.Count -gt 0) { $facts.Address = $m[$m.Count - 1].Groups[1].Value }
    $f = [regex]::Matches($text, 'Firmware Version:\s*([0-9]+\.[0-9]+\.[0-9]+)')
    if ($f.Count -gt 0) { $facts.Firmware = $f[$f.Count - 1].Groups[1].Value }
    return $facts
}

function Get-ErrorLogLastLine {
    $err = Join-Path $LogDir 'esp32gw.err.log'
    if (-not (Test-Path -LiteralPath $err)) { return '' }
    $lines = @()
    try { $lines = @(Get-Content -LiteralPath $err -ErrorAction Stop | Where-Object { $_.Trim() }) } catch { return '' }
    if ($lines.Count -eq 0) { return '' }
    $last = $lines[$lines.Count - 1].Trim()
    if ($last.Length -gt 120) { $last = $last.Substring(0, 120) }
    return $last
}

# ---- 自动重连: 网关被重启后, 把上次连过的板子 IP 自动重发一遍 ----------------
# 网关(esp32gw)启动后要等第一条 ip_address 报文才会去连板子。所以每次它被
# 重启(看门狗修复 / 进程崩了 / 开机), Scratch 那边就得重新点一次 IP 积木。
# 这里由守护进程代劳: 记住最后一次连成功的 IP, 新实例起来 3 秒后自动重发。
# 网关启动时还没订阅上主题, 第一条 ip_address 有可能会被 PUB/SUB 丢掉, 所以
# 发出去之后过 connectRetrySeconds 还没连上就补发一次, 试满 maxAutoConnectAttempts
# 次为止 (之后停下, 等用户手动点积木)。
$script:boardIpFile = Join-Path $LogDir 'last_board_ip.txt'
$script:lastGatewayPid = 0
$script:pendingConnectAt = $null
$script:pendingConnectIp = ''
$script:autoConnectAttempts = 0
$script:maxAutoConnectAttempts = 6
$script:connectRetrySeconds = 4
$script:autoConnectGaveUpLogged = $false
# 板子没连上时, 每隔这么久自动发现一次 (板子后上电 / 换了 IP 都能自己连上)
$script:discoveryIntervalSeconds = 15
$script:nextDiscoveryAt = $null
$script:linkCheckCounter = 0

function Get-RememberedBoardIp {
    if (Test-Path -LiteralPath $script:boardIpFile) {
        $value = (Get-Content -LiteralPath $script:boardIpFile -ErrorAction SilentlyContinue |
            Select-Object -First 1)
        if ($value) { return $value.Trim() }
    }
    return ''
}

function Set-RememberedBoardIp {
    param([string]$Address)
    if (-not $Address) { return }
    try { Set-Content -LiteralPath $script:boardIpFile -Value $Address -Encoding ASCII } catch { }
}

function Send-BoardIpCommand {
    param([string]$Address)
    if (-not $script:pythonExe -or -not (Test-Path -LiteralPath $script:publishScript)) { return $false }
    try {
        & $script:pythonExe $script:publishScript -t 'to_esp32_gateway' -c 'ip_address' --address $Address 2>&1 |
            Out-Null
        return $true
    } catch {
        return $false
    }
}

# 板子被 DHCP 换了地址时, 扫一遍局域网把它找出来
function Find-BoardAddress {
    $finder = Join-Path $PSScriptRoot 'find_board.py'
    if (-not $script:pythonExe -or -not (Test-Path -LiteralPath $finder)) { return '' }
    try {
        # 先听板子的 UDP 广播 (探测包一发一收, 通常 100ms 内就有结果),
        # 收不到再退回扫网段, 所以 --beacon-timeout 给短一点就够
        $output = & $script:pythonExe $finder --first --beacon-timeout 1.5 2>&1
        $ip = $output | Where-Object { $_ -match '^\s*\d+\.\d+\.\d+\.\d+\s*$' } | Select-Object -First 1
        if ($ip) { return $ip.ToString().Trim() }
    } catch {
        Write-Log "扫描板子失败: $($_.Exception.Message)"
    }
    return ''
}

Write-Log "守护进程启动 (间隔 ${IntervalSeconds}s, 脚本目录 $scriptDir)"
$remembered = Get-RememberedBoardIp
if ($remembered) {
    Write-Log "记住的板子 IP: $remembered (网关重启后会自动重发)"
}

while ($true) {
    # backplane 在跑, 但监听的地址已经不属于本机 (PC 换网络 / DHCP 换 IP) ->
    # 谁都连不上它, 这种"僵尸 backplane"自己不会重绑, 只能重启; 两个网关也一起
    # 重启, 让它们按当前地址重新解析并重新订阅。
    if ((Get-Process backplane -ErrorAction SilentlyContinue) -and -not (Test-BackplaneBind)) {
        Write-Log "backplane 监听的地址已不属于本机 (本机 IP 变过), 重启 backplane 和两个网关"
        Stop-Component -Name 'esp32gw'
        Stop-Component -Name 'wsgw'
        Stop-Component -Name 'backplane'
        Start-Sleep -Seconds 1
    }

    if (-not (Test-Port 43124)) {
        if (-not (Get-Process backplane -ErrorAction SilentlyContinue)) {
            Start-Component -Name 'backplane'
            Start-Sleep -Seconds 4      # 等 Banyan 真正就绪
        }
    }

    $gwProc = Get-Process esp32gw -ErrorAction SilentlyContinue

    if (Test-Port 43124) {
        if (-not (Get-Process wsgw -ErrorAction SilentlyContinue)) {
            Start-Component -Name 'wsgw' -Arguments @('-i', '9007')
        }
        if (-not $gwProc) {
            # 先看看它是怎么没的: 有堆栈就是连板子失败, 没堆栈就是正常没在跑
            $errLine = Get-ErrorLogLastLine
            if ($errLine) {
                Write-Log "网关退出前的错误行: $errLine"
                Publish-GatewayStatus -State 'error' -Message '网关连板子失败, 正在自动重连'
            } else {
                Publish-GatewayStatus -State 'waiting' -Message '网关未运行' -Force
            }
            Start-Component -Name 'esp32gw'
        } elseif ((Get-ErrorLogSize 'esp32gw') -gt [int]$errorLogBaseline['esp32gw']) {
            # 进程在, 但接收循环已经死了 -> 重启
            Test-Esp32GatewayHealth
        } elseif ($script:linkCheckCounter -ge 5) {
            # 每 5 个循环查一次真实连接: 日志说已连接但这会儿没有 TCP 连接 -> 僵尸网关
            $script:linkCheckCounter = 0
            $linkFacts = Get-GatewayLogFacts
            if ($linkFacts.Address -and
                -not (Test-BoardLinkAlive -Address $linkFacts.Address)) {
                Write-Log "板子的 TCP 连接已经不在了 (日志仍写着已连接 $($linkFacts.Address)), 重启网关"
                Test-Esp32GatewayHealth
            }
        }
        $script:linkCheckCounter++
        $gwProc = Get-Process esp32gw -ErrorAction SilentlyContinue
    }

    # ---- 新实例 -> 安排自动重发上次的板子 IP ----
    if ($gwProc) {
        if ($gwProc.Id -ne $script:lastGatewayPid) {
            $script:lastGatewayPid = $gwProc.Id
            $script:pendingConnectAt = $null
            # 这个实例要是已经连上板子了(例如只是守护进程自己重启), 就不用重发
            $alreadyConnected = (Get-GatewayLogFacts).Address
            if ($alreadyConnected) {
                Write-Log "网关实例 (PID $($gwProc.Id)) 已连上板子 $alreadyConnected, 无需自动重发"
            } elseif (-not $DisableAutoReconnect) {
                $target = Get-RememberedBoardIp
                # 没有历史 IP, 或者上次地址连不上: 先自动发现一遍。
                # find_board.py 会优先听板子的 UDP 广播 (毫秒级), 收不到再扫网段,
                # 所以板子换了 DHCP 地址 / 第一次上电都不用去串口抄 IP。
                if (-not $target -or $script:autoConnectAttempts -ge 1) {
                    Write-Log "自动发现板子地址 (先听 UDP 广播, 收不到再扫网段)..."
                    $found = Find-BoardAddress
                    if ($found) {
                        if ($found -ne $target) {
                            Write-Log "发现板子在 $found, 改用这个地址 (并记住它)"
                            Set-RememberedBoardIp -Address $found
                        } else {
                            Write-Log "发现确认板子还是在 $found"
                        }
                        $target = $found
                    } elseif ($target) {
                        Write-Log "没发现新地址, 继续用记住的 IP $target"
                    } else {
                        Write-Log "没有发现板子 (没上电 / 不在同一网段 / 广播被挡?)"
                    }
                }
                if ($target -and $script:autoConnectAttempts -lt $script:maxAutoConnectAttempts) {
                    $script:pendingConnectIp = $target
                    $script:pendingConnectAt = (Get-Date).AddSeconds(3)
                    $script:autoConnectGaveUpLogged = $false
                    Write-Log ("新网关实例 (PID {0}): 3 秒后自动重发板子 IP {1} (第 {2} 次)" -f `
                        $gwProc.Id, $target, ($script:autoConnectAttempts + 1))
                } elseif ($target -and -not $script:autoConnectGaveUpLogged) {
                    $script:autoConnectGaveUpLogged = $true
                    Write-Log "自动重连已试 $($script:maxAutoConnectAttempts) 次仍未成功, 暂停 (在 Scratch 里点一次「连接板子 IP」即可)"
                    Publish-GatewayStatus -State 'error' -Force `
                        -Message '板子连不上, 已停止自动重连 (点一次「连接板子 IP」再试)'
                }
            }
        }
        # 到点就发; 发完还没连上, 过 connectRetrySeconds 再补发一次
        if ($script:pendingConnectAt -and (Get-Date) -ge $script:pendingConnectAt) {
            $ip = $script:pendingConnectIp
            if ((Get-GatewayLogFacts).Address) {
                # 已经连上了, 不用再发
                $script:pendingConnectAt = $null
            } elseif ($script:autoConnectAttempts -ge $script:maxAutoConnectAttempts) {
                $script:pendingConnectAt = $null
            } else {
                $script:autoConnectAttempts++
                if (Send-BoardIpCommand -Address $ip) {
                    Write-Log "已自动重发 ip_address $ip (第 $($script:autoConnectAttempts) 次)"
                    $script:pendingConnectAt = (Get-Date).AddSeconds($script:connectRetrySeconds)
                } else {
                    Write-Log "自动重发 ip_address 失败 (没找到 python / banyan_publish.py)"
                    $script:pendingConnectAt = $null
                }
            }
        }
    } else {
        $script:lastGatewayPid = 0
    }

    # ---- 把当前状态报给 Scratch ----
    $script:heartbeatCount++
    if ($gwProc) {
        $facts = Get-GatewayLogFacts
        if ($facts.Address) {
            # 连上了: 记住这个 IP, 并清掉自动重连的计数
            if ($facts.Address -ne (Get-RememberedBoardIp)) { Set-RememberedBoardIp -Address $facts.Address }
            if ($script:autoConnectAttempts -ne 0) {
                Write-Log "板子已连上 ($($facts.Address)), 自动重连计数清零"
                $script:autoConnectAttempts = 0
            }
            $script:pendingConnectAt = $null
            Publish-GatewayStatus -State 'connected' -Address $facts.Address -Firmware $facts.Firmware
            $script:nextDiscoveryAt = $null
        } else {
            Publish-GatewayStatus -State 'waiting' -Message '点 Scratch 里的「连接板子 IP」'
        }
    }

    # ---- 板子还没连上时, 定期自动发现 (板子后上电 / DHCP 换地址都能自己连上) ----
    if ($gwProc -and -not $DisableAutoReconnect -and
        -not (Get-GatewayLogFacts).Address) {
        if (-not $script:nextDiscoveryAt -or (Get-Date) -ge $script:nextDiscoveryAt) {
            $script:nextDiscoveryAt = (Get-Date).AddSeconds($script:discoveryIntervalSeconds)
            $found = Find-BoardAddress
            if ($found) {
                Write-Log "自动发现: 板子在 $found, 直接发 ip_address 连它"
                Set-RememberedBoardIp -Address $found
                $script:pendingConnectIp = $found
                $script:pendingConnectAt = $null
                if (Send-BoardIpCommand -Address $found) {
                    $script:autoConnectAttempts = 0
                }
            }
        }
    }

    # 心跳: 每 10 个循环(约 12 秒)重发一次当前状态, 这样后打开 Scratch 也能看到状态。
    # 注意别调太小: 每次上报都要起一个 python 进程(约 0.9 秒), 太密会把巡检本身拖慢。
    if ($script:heartbeatCount -ge 10 -and $script:lastStatus.Count -gt 0) {
        $script:heartbeatCount = 0
        Publish-GatewayStatus -State $script:lastStatus.State -Address $script:lastStatus.Address `
            -Firmware $script:lastStatus.Firmware -Message $script:lastStatus.Message -Force
    }

    Start-Sleep -Seconds $IntervalSeconds
}
