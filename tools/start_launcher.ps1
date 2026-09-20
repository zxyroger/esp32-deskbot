<#
.SYNOPSIS
    启动 "onegpio 按需启动器" —— 一个几十 MB 的小进程, 让"点积木自动拉起服务"成立。

.DESCRIPTION
    Scratch / TurboWarp 的扩展跑在浏览器沙箱里, 不能启动本机进程, 只能发
    HTTP / WebSocket。所以做法是:

        Scratch 扩展 ──HTTP /start──► 启动器 (本脚本起的常驻小进程, 127.0.0.1:8000)
                                          │
                                          ▼
                                    start_s3extend.ps1 -Background
                                          │
                                          ▼
                              守护进程 + backplane + wsgw(9007) + esp32gw

    启动器本身:
      * 只用 Python 标准库, 不 import s3-extend (s3-extend 没装好也能起来);
      * 不占 9007 / 43124 / 43125, 内存十几 MB, 常驻没有负担;
      * 顺便把 scratch\esp32s3.js 挂在 http://127.0.0.1:8000/esp32s3.js
        (TurboWarp 的自定义扩展 URL 可以直接填这个)。

    用法:
      .\start_launcher.ps1             # 后台隐藏启动 (默认)
      .\start_launcher.ps1 -Check      # 看启动器 + 服务状态
      .\start_launcher.ps1 -Foreground # 前台运行, 日志直接打在窗口里
      .\start_launcher.ps1 -Stop       # 停掉启动器 (不影响正在跑的服务)
      .\start_launcher.ps1 -Restart    # 重启启动器
      .\start_launcher.ps1 -AllowOrigin https://my.editor
                                       # 额外放行一个来源 (可重复)
                                       # 默认已放行 turbowarp.org / scratch.mit.edu
                                       # / penguinmod.com / adacraft.org 和本机来源

    登录后自动常驻它 (这样重启 PC 之后点积木就能拉起服务):
      .\install_autostart.ps1 -OnDemand

.EXAMPLE
    .\start_launcher.ps1
#>
[CmdletBinding()]
param(
    [int]$Port = 8000,
    [switch]$Check,
    [switch]$Stop,
    [switch]$Restart,
    [switch]$Foreground,
    [string[]]$AllowOrigin
)

$ErrorActionPreference = 'Stop'

$toolsDir = $PSScriptRoot
$cliExtra = @()
foreach ($origin in $AllowOrigin) {
    if ($origin) { $cliExtra += @('--allow-origin', $origin) }
}
$launcherScript = Join-Path $toolsDir 'onegpio_launcher.py'
$logDir = Join-Path $env:LOCALAPPDATA 's3extend\logs'
$logFile = Join-Path $logDir 'launcher.log'
$errFile = Join-Path $logDir 'launcher.err.log'
$pidFile = Join-Path $env:LOCALAPPDATA 's3extend\launcher.pid'
$statusUrl = "http://127.0.0.1:$Port/status"

function Find-Python {
    param([switch]$Windowless)

    $exe = if ($Windowless) { 'pythonw.exe' } else { 'python.exe' }
    $candidates = @()

    # 1) PATH 里的 python, 换成同目录的 pythonw
    $onPath = Get-Command python -ErrorAction SilentlyContinue
    if ($onPath -and $onPath.Source -notmatch 'WindowsApps') {
        $candidates += (Join-Path (Split-Path -Parent $onPath.Source) $exe)
    }
    # 2) 常见安装位置
    $candidates += @(
        "C:\Program Files\Python313\$exe",
        "C:\Program Files\Python312\$exe",
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python313\$exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\$exe")
    )
    # 3) 最后退回 PATH
    $found = Get-Command $exe -ErrorAction SilentlyContinue
    if ($found -and $found.Source -notmatch 'WindowsApps') { $candidates += $found.Source }

    foreach ($path in $candidates) {
        if ($path -and (Test-Path -LiteralPath $path)) { return $path }
    }
    return $null
}

function Get-LauncherProcess {
    $found = @()
    if (Test-Path -LiteralPath $pidFile) {
        $text = (Get-Content -LiteralPath $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($text -match '^\d+$') {
            $proc = Get-Process -Id ([int]$text) -ErrorAction SilentlyContinue
            if ($proc -and $proc.ProcessName -match 'python') { $found += $proc }
        }
    }
    if ($found.Count -eq 0) {
        # PID 文件丢了就按命令行找 (只认启动器自己的脚本)
        $found = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -match 'onegpio_launcher\.py' } |
            ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
    }
    return $found
}

function Test-Launcher {
    return (Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue) -ne $null
}

function Test-Service {
    return (Get-NetTCPConnection -State Listen -LocalPort 9007 -ErrorAction SilentlyContinue) -ne $null
}

if (-not (Test-Path -LiteralPath $launcherScript)) {
    Write-Host "找不到启动器脚本: $launcherScript" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if ($Stop -or $Restart) {
    $procs = Get-LauncherProcess
    if ($procs.Count -eq 0) {
        Write-Host "启动器没有在运行。" -ForegroundColor Yellow
    } else {
        foreach ($proc in $procs) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            Write-Host "已停止启动器 (PID $($proc.Id))。" -ForegroundColor Green
        }
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    if ($Stop) { exit 0 }
}

if ($Check) {
    $launcherUp = Test-Launcher
    Write-Host "---- 启动器 ----" -ForegroundColor Cyan
    if ($launcherUp) {
        Write-Host "  运行中: http://127.0.0.1:$Port/  (扩展 URL /esp32s3.js)" -ForegroundColor Green
        try {
            $status = Invoke-RestMethod -Uri $statusUrl -TimeoutSec 5
            Write-Host ("  PID {0}, 已运行 {1} 秒, 累计拉起服务 {2} 次" -f `
                $status.launcher.pid, $status.launcher.uptime_s, $status.start.start_count)
        } catch {
            Write-Host "  (端口在监听, 但 /status 没应答: $($_.Exception.Message))" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  未运行 -> 启动: $toolsDir\start_launcher.ps1" -ForegroundColor Red
    }

    Write-Host "---- s3-extend 服务 ----" -ForegroundColor Cyan
    if (Test-Service) {
        Write-Host "  运行中 (9007 已监听): 点积木直接就能用。" -ForegroundColor Green
    } else {
        Write-Host "  未运行: 点任意积木时, 扩展会请启动器把它拉起来。" -ForegroundColor Yellow
    }
    Write-Host "---- 登录自启 ----" -ForegroundColor Cyan
    & (Join-Path $toolsDir 'install_autostart.ps1') -Status
    exit 0
}

if (Test-Launcher) {
    if (-not $Restart) {
        Write-Host "启动器已经在运行 (端口 $Port 已监听), 跳过启动。" -ForegroundColor Yellow
        Write-Host "  状态: .\start_launcher.ps1 -Check"
        exit 0
    }
    Write-Host "端口 $Port 仍被占用, 请稍后重试或换 -Port。" -ForegroundColor Red
    exit 1
}

if ($Foreground) {
    $python = Find-Python
    if (-not $python) {
        Write-Host "找不到 python.exe, 无法运行启动器。" -ForegroundColor Red
        exit 1
    }
    Write-Host "前台运行启动器 (Ctrl+C 停止) ..." -ForegroundColor Cyan
    & $python $launcherScript --port $Port @cliExtra
    exit $LASTEXITCODE
}

$pythonw = Find-Python -Windowless
if (-not $pythonw) {
    Write-Host "找不到 pythonw.exe, 无法后台启动 (可以用 -Foreground 前台跑)。" -ForegroundColor Red
    exit 1
}

Write-Host "启动 onegpio 启动器: $pythonw $launcherScript --port $Port" -ForegroundColor Cyan
$proc = Start-Process -FilePath $pythonw `
    -ArgumentList (@($launcherScript, '--port', "$Port") + $cliExtra) `
    -RedirectStandardOutput $logFile `
    -RedirectStandardError $errFile `
    -WindowStyle Hidden -PassThru

Set-Content -LiteralPath $pidFile -Value $proc.Id -Encoding ASCII

# 等它把 8000 端口挂上
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    if (Test-Launcher) { break }
    Start-Sleep -Milliseconds 300
}

if (Test-Launcher) {
    Write-Host "启动器已就绪 (PID $($proc.Id))。" -ForegroundColor Green
    Write-Host "  状态页   : http://127.0.0.1:$Port/"
    Write-Host "  扩展 URL : http://127.0.0.1:$Port/esp32s3.js"
    if (Test-Service) {
        Write-Host "  s3-extend 已在运行。" -ForegroundColor Green
    } else {
        Write-Host "  s3-extend 还没起来: 在 Scratch 里点任意积木就会自动拉起。" -ForegroundColor Yellow
    }
    Write-Host "  日志     : $logFile"
} else {
    Write-Host "20 秒内端口 $Port 没起来, 看日志: $logFile / $errFile" -ForegroundColor Red
    Write-Host "也可以前台跑一次看报错: .\start_launcher.ps1 -Foreground" -ForegroundColor Yellow
    exit 1
}
