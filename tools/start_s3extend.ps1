<#
.SYNOPSIS
    一键启动 s3-extend 的 ESP32 扩展服务器 (backplane + wsgw:9007 + esp32gw)

.DESCRIPTION
    本机是微软商店版 Python, 脚本目录 (%APPDATA%\Python\Python313\Scripts)
    默认不在 PATH 里, 直接敲 s32 会报 FileNotFoundError: 'backplane'。
    这个脚本会把脚本目录临时加进本次会话的 PATH, 再启动 s32。

    启动后: 在 Scratch 里加载 OneGpio ESP32 扩展并填写板子 IP 即可。
    停止: 在这个窗口按 Ctrl+C (会自动结束 backplane / wsgw / esp32gw)。

.EXAMPLE
    .\start_s3extend.ps1
    .\start_s3extend.ps1 -Check      # 只看环境与端口占用, 不启动
    .\start_s3extend.ps1 -Logs       # 不用 s32 启动器, 三个组件日志直接打在窗口里
    .\start_s3extend.ps1 -Background # 后台静默启动, 日志写文件 (开机自启用这个)
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Logs,
    [switch]$Background
)

$ErrorActionPreference = 'Stop'

function Find-S3Script {
    param([string]$Name)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $candidates = @(
        (Join-Path $env:APPDATA "Python\Python313\Scripts\$Name.exe"),
        (Join-Path $env:APPDATA "Python\Python312\Scripts\$Name.exe"),
        (Join-Path $env:APPDATA "Python\Python311\Scripts\$Name.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python313\Scripts\$Name.exe")
    )
    foreach ($path in $candidates) {
        if (Test-Path $path) { return $path }
    }
    return $null
}

$s32 = Find-S3Script 's32'
if (-not $s32) {
    Write-Host "找不到 s32，说明 s3-extend 还没装好。" -ForegroundColor Red
    Write-Host "安装命令: python -m pip install --user s3-extend" -ForegroundColor Yellow
    exit 1
}

$scriptDir = Split-Path -Parent $s32
Write-Host "s3-extend 启动器: $s32" -ForegroundColor Cyan

# 关键一步: 让 s32 能找到 backplane / wsgw / esp32gw
$env:Path = "$scriptDir;$env:Path"

foreach ($tool in 'backplane', 'wsgw', 'esp32gw') {
    $found = Find-S3Script $tool
    if (-not $found) {
        Write-Host "缺少 $tool，s3-extend 安装不完整。" -ForegroundColor Red
        exit 1
    }
}

if ($Check) {
    Write-Host "环境检查通过 (backplane / wsgw / esp32gw 都能找到)。" -ForegroundColor Green

    Write-Host ""
    Write-Host "---- 当前运行状态 ----" -ForegroundColor Cyan
    $procs = Get-Process backplane, wsgw, esp32gw, s32 -ErrorAction SilentlyContinue
    foreach ($name in 's32', 'backplane', 'wsgw', 'esp32gw') {
        if ($procs | Where-Object ProcessName -eq $name) {
            Write-Host ("  {0,-10} 运行中" -f $name) -ForegroundColor Green
        } else {
            Write-Host ("  {0,-10} 未运行" -f $name) -ForegroundColor DarkGray
        }
    }

    $ports = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
        Where-Object { $_.LocalPort -in 9007, 43124, 43125 }
    foreach ($port in 43124, 43125, 9007) {
        $item = $ports | Where-Object LocalPort -eq $port
        if ($item) {
            Write-Host ("  端口 {0} 已监听 ({1})" -f $port, $item[0].LocalAddress) -ForegroundColor Green
        } else {
            Write-Host ("  端口 {0} 未监听" -f $port) -ForegroundColor Red
        }
    }

    # esp32gw 可能"进程活着但接收循环已死": 上游网关的 Banyan 接收循环没有异常保护,
    # 板子重启 / WiFi 掉线留下的一次 ConnectionResetError 就会把它打死,
    # 之后进程还在, 但再也不响应积木指令 (表现: 点什么都没反应)。
    # 这种情况下 stderr 里会留下堆栈, 所以用错误日志是否非空来判断。
    $gwErrLog = Join-Path $env:LOCALAPPDATA 's3extend\logs\esp32gw.err.log'
    $gwErrSize = if (Test-Path -LiteralPath $gwErrLog) { (Get-Item -LiteralPath $gwErrLog).Length } else { 0 }
    if ($procs | Where-Object ProcessName -eq 'esp32gw') {
        if ($gwErrSize -gt 0) {
            Write-Host "  esp32gw 健康检查: 已失效 (错误日志里有未处理异常)" -ForegroundColor Red
            Write-Host "      积木会全部没反应。重启网关即可 (守护进程 1~4 秒内自动拉起新实例):" -ForegroundColor Yellow
            Write-Host "        Get-Process esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force" -ForegroundColor Gray
        } else {
            Write-Host "  esp32gw 健康检查: 正常" -ForegroundColor Green
        }
    }

    Write-Host ""
    if ($ports | Where-Object LocalPort -eq 9007) {
        Write-Host "结论: s3-extend 已就绪, 去 Scratch 里点「连接板子 IP」。"
        Write-Host "      (连接是否成功, 用 -Logs 启动或看板子串口日志确认)"
        Write-Host "      提醒: 端口被占用时本脚本无法再启动一套, 先停掉旧的。"
    } else {
        Write-Host "结论: 9007 没有监听 -> wsgw 没在运行, Scratch 扩展会连不上。" -ForegroundColor Red
        Write-Host "      修复: 先收掉残留进程, 再重新启动 (建议带日志):" -ForegroundColor Yellow
        Write-Host "        Get-Process s32,backplane,wsgw,esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force" -ForegroundColor Gray
        Write-Host "        D:\esp\onegpio\tools\start_s3extend.ps1 -Logs" -ForegroundColor Gray
    }
    exit 0
}

if ($Logs) {
    # 直接用 job 起三个组件, 这样它们的日志 (例如 "Successfully connected to: ...")
    # 会实时打在当前窗口里, 排查"连接板子是否成功"时非常有用。
    Write-Host "启动 backplane / wsgw(9007) / esp32gw, 日志实时显示 (Ctrl+C 停止) ..." -ForegroundColor Cyan

    $scriptDirLiteral = $scriptDir
    $jobs = @(
        Start-Job -Name backplane -ScriptBlock {
            param($d)
            $env:Path = "$d;$env:Path"
            & "$d\backplane.exe"
        } -ArgumentList $scriptDirLiteral
        Start-Job -Name wsgw -ScriptBlock {
            param($d)
            $env:Path = "$d;$env:Path"
            & "$d\wsgw.exe" -i 9007
        } -ArgumentList $scriptDirLiteral
        Start-Job -Name esp32gw -ScriptBlock {
            param($d)
            $env:Path = "$d;$env:Path"
            & "$d\esp32gw.exe"
        } -ArgumentList $scriptDirLiteral
    )

    try {
        while ($true) {
            foreach ($job in $jobs) {
                Receive-Job -Job $job -ErrorAction SilentlyContinue |
                    ForEach-Object { Write-Host "[$($job.Name)] $_" }
            }
            Start-Sleep -Milliseconds 400
        }
    } finally {
        $jobs | Stop-Job -ErrorAction SilentlyContinue
        $jobs | Remove-Job -Force -ErrorAction SilentlyContinue
        # 顺手收掉可能残留的子进程
        Get-Process backplane, wsgw, esp32gw -ErrorAction SilentlyContinue | Stop-Process -Force
        Write-Host "已停止 s3-extend 组件。" -ForegroundColor Yellow
    }
    exit 0
}

if ($Background) {
    # 后台静默启动: 拉起守护进程, 由它保证三个组件一直活着。
    # 日志写到 %LOCALAPPDATA%\s3extend\logs, 供 install_autostart.ps1 调用。
    $logDir = Join-Path $env:LOCALAPPDATA 's3extend\logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    # 已经在跑就不重复启动 (用 PID 文件精确判断)
    $pidFile = Join-Path $logDir 'supervisor.pid'
    if (Test-Path $pidFile) {
        $oldPid = (Get-Content -LiteralPath $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        $proc = if ($oldPid) { Get-Process -Id ([int]$oldPid) -ErrorAction SilentlyContinue } else { $null }
        if ($proc -and $proc.ProcessName -eq 'powershell') {
            Write-Host "守护进程已经在运行 (PID $oldPid), 跳过启动。" -ForegroundColor Yellow
            exit 0
        }
    }
    if (Get-NetTCPConnection -State Listen -LocalPort 9007 -ErrorAction SilentlyContinue) {
        Write-Host "9007 已在监听 (可能通过其他方式启动), 跳过。" -ForegroundColor Yellow
        exit 0
    }

    $supervisorScript = Join-Path $PSScriptRoot 'supervise_s3extend.ps1'
    Start-Process -FilePath 'powershell.exe' `
        -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden',
                      '-File', $supervisorScript, '-LogDir', $logDir `
        -WindowStyle Hidden | Out-Null

    # 等它把服务拉起来
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline) {
        if (Get-NetTCPConnection -State Listen -LocalPort 9007 -ErrorAction SilentlyContinue) {
            Write-Host "s3-extend 已在后台运行 (守护进程 + backplane/wsgw/esp32gw)。" -ForegroundColor Green
            Write-Host "日志目录: $logDir"
            exit 0
        }
        Start-Sleep -Milliseconds 500
    }

    Write-Host "40 秒内 9007 仍未监听, 请查看 $logDir\supervisor.log" -ForegroundColor Red
    exit 1
}

Write-Host "启动 backplane + wsgw(9007) + esp32gw ..." -ForegroundColor Cyan
Write-Host "停止请按 Ctrl+C" -ForegroundColor Yellow
& $s32
