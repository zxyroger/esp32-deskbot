<#
.SYNOPSIS
    把 s3-extend 注册成"登录后自动在后台运行"的计划任务。

.DESCRIPTION
    两种模式:
      * 默认 (全量): 登录后直接把 s3-extend 三件套拉起来
        (backplane + wsgw:9007 + esp32gw, 由守护进程看着);
      * -OnDemand (按需): 登录后只常驻"微型启动器" (一个 Python 小进程,
        127.0.0.1:8000), 真正重的三件套等你在 Scratch 里点积木时才拉起
        —— 也就是"点积木自动拉起服务"。

    两种落地方式 (默认自动选):
      1) 计划任务 "s3-extend ESP32 server" (登录时触发, 隐藏窗口)
         —— 需要以管理员身份运行 PowerShell;
      2) 启动文件夹里的 s3extend_autostart.vbs (登录时用 wscript 隐藏启动)
         —— 不需要管理员权限。

    两者都不弹窗, 日志写到 %LOCALAPPDATA%\s3extend\logs\。
    安装某种模式时会自动清掉另一种模式的登录项, 避免两套同时常驻。

    为什么是"登录时"而不是"开机时":
      s3-extend 装在当前用户的 %APPDATA% 下, 以 SYSTEM 身份在开机阶段运行
      会找不到那些命令 (backplane / wsgw / esp32gw)。登录触发等价于"开机后
      你自己一登录, 服务就自动起来"。

.EXAMPLE
    .\install_autostart.ps1                  # 安装 (自动选可用方式)
    .\install_autostart.ps1 -OnDemand        # 只常驻微型启动器, 点积木时才拉起服务
    .\install_autostart.ps1 -Mode Startup    # 强制用启动文件夹
    .\install_autostart.ps1 -Mode Task       # 强制用计划任务 (需管理员)
    .\install_autostart.ps1 -Status          # 看是否已安装
    .\install_autostart.ps1 -Remove          # 卸载
#>
[CmdletBinding()]
param(
    [switch]$Remove,
    [switch]$Status,
    [switch]$OnDemand,
    [ValidateSet('Auto', 'Task', 'Startup')]
    [string]$Mode = 'Auto'
)

$ErrorActionPreference = 'Stop'

$fullTaskName = 's3-extend ESP32 server'
$launcherTaskName = 'onegpio launcher (on demand)'
$scriptPath = Join-Path $PSScriptRoot 'start_s3extend.ps1'
$launcherPath = Join-Path $PSScriptRoot 'start_launcher.ps1'
$startupDir = [Environment]::GetFolderPath('Startup')
$vbsPath = Join-Path $startupDir 's3extend_autostart.vbs'
$launcherVbsPath = Join-Path $startupDir 'onegpio_launcher_autostart.vbs'

if ($OnDemand) {
    $taskName = $launcherTaskName
    $targetScript = $launcherPath
    $targetArgs = ''
    $targetVbs = $launcherVbsPath
    $otherTaskName = $fullTaskName
    $otherVbs = $vbsPath
    $modeText = '按需模式: 只常驻微型启动器, 点积木时自动拉起 s3-extend'
} else {
    $taskName = $fullTaskName
    $targetScript = $scriptPath
    $targetArgs = '-Background'
    $targetVbs = $vbsPath
    $otherTaskName = $launcherTaskName
    $otherVbs = $launcherVbsPath
    $modeText = '全量模式: 登录后直接把 s3-extend 三件套拉起来'
}

function Get-LogDir { Join-Path $env:LOCALAPPDATA 's3extend\logs' }

function New-StartupEntry {
    # wscript 用 0 号窗口模式启动, 完全无窗口闪现
    param([string]$ScriptPath, [string]$ScriptArgs, [string]$VbsPath)
    $command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""$ScriptPath"""
    if ($ScriptArgs) { $command += " $ScriptArgs" }
    $content = 'CreateObject("WScript.Shell").Run "' + $command.Replace('"', '""') + '", 0, False'
    Set-Content -LiteralPath $VbsPath -Value $content -Encoding ASCII
}

if ($Status) {
    $found = $false
    foreach ($item in @(
            @{ Name = $fullTaskName;     Text = '全量模式 (登录后直接起三件套)' },
            @{ Name = $launcherTaskName; Text = '按需模式 (登录后只起微型启动器)' })) {
        $task = Get-ScheduledTask -TaskName $item.Name -ErrorAction SilentlyContinue
        if ($task) {
            $found = $true
            Write-Host "[计划任务] $($item.Text): $($item.Name) (状态 $($task.State))" -ForegroundColor Green
        }
    }
    foreach ($item in @(
            @{ Path = $vbsPath;         Text = '全量模式 (登录后直接起三件套)' },
            @{ Path = $launcherVbsPath; Text = '按需模式 (登录后只起微型启动器)' })) {
        if (Test-Path $item.Path) {
            $found = $true
            Write-Host "[启动文件夹] $($item.Text): $($item.Path)" -ForegroundColor Green
        }
    }
    if (-not $found) {
        Write-Host "未安装登录自启 (两种模式都没有)。" -ForegroundColor Yellow
    }
    Write-Host "日志目录: $(Get-LogDir)"
    exit 0
}

if ($Remove) {
    $removed = $false
    foreach ($name in @($fullTaskName, $launcherTaskName)) {
        if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $name -Confirm:$false
            Write-Host "已删除计划任务 '$name'。" -ForegroundColor Green
            $removed = $true
        }
    }
    foreach ($path in @($vbsPath, $launcherVbsPath)) {
        if (Test-Path $path) {
            Remove-Item -LiteralPath $path -Force
            Write-Host "已删除启动项 '$path'。" -ForegroundColor Green
            $removed = $true
        }
    }
    if (-not $removed) {
        Write-Host "没有找到任何开机自启项。" -ForegroundColor Yellow
    }
    Write-Host "注意: 正在运行的 s3-extend 不受影响, 要停它请运行 stop_s3extend.ps1。"
    exit 0
}

if (-not (Test-Path $scriptPath) -or ($OnDemand -and -not (Test-Path $launcherPath))) {
    Write-Host "找不到脚本: $targetScript" -ForegroundColor Red
    exit 1
}

function Install-Task {
    param([string]$ScriptPath, [string]$ScriptArgs, [string]$Description)
    $argument = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$ScriptPath`""
    if ($ScriptArgs) { $argument += " $ScriptArgs" }
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument $argument
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
        -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero)

    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
        -Settings $settings `
        -Description $Description `
        -Force | Out-Null
}

$installed = $false

if ($OnDemand) {
    $taskDescription = "登录后常驻 onegpio 微型启动器 (127.0.0.1:8000); 点积木时由它拉起 s3-extend (backplane + wsgw:9007 + esp32gw)。"
} else {
    $taskDescription = "登录后在后台启动 s3-extend (backplane + wsgw:9007 + esp32gw), 供 Scratch 控制 ESP32-S3。"
}

# 两套模式互斥: 装当前模式前, 先把另一种模式的登录项摘掉
$otherFound = $false
if (Get-ScheduledTask -TaskName $otherTaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $otherTaskName -Confirm:$false
    Write-Host "已移除另一种模式的计划任务 '$otherTaskName'。" -ForegroundColor Yellow
    $otherFound = $true
}
if (Test-Path $otherVbs) {
    Remove-Item -LiteralPath $otherVbs -Force
    Write-Host "已移除另一种模式的启动项 '$otherVbs'。" -ForegroundColor Yellow
    $otherFound = $true
}
if ($otherFound) { Write-Host "" }

if ($Mode -eq 'Auto' -or $Mode -eq 'Task') {
    try {
        Install-Task -ScriptPath $targetScript -ScriptArgs $targetArgs -Description $taskDescription
        Write-Host "已安装计划任务 '$taskName' (登录时隐藏窗口启动)。" -ForegroundColor Green
        $installed = $true
    } catch {
        if ($Mode -eq 'Task') {
            Write-Host "注册计划任务失败: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "请用【以管理员身份运行】的 PowerShell 再试一次, 或改用 -Mode Startup。" -ForegroundColor Yellow
            exit 1
        }
        Write-Host "注册计划任务失败 (通常是没有以管理员身份运行), 改用启动文件夹方式。" -ForegroundColor Yellow
    }
}

if (-not $installed) {
    New-StartupEntry -ScriptPath $targetScript -ScriptArgs $targetArgs -VbsPath $targetVbs
    Write-Host "已安装启动项: $targetVbs" -ForegroundColor Green
    Write-Host "登录时会用 wscript 隐藏窗口启动, 不会闪现命令行窗口。"
}

Write-Host "模式: $modeText" -ForegroundColor Cyan
Write-Host "启动脚本: $targetScript $targetArgs"
Write-Host "日志目录: $(Get-LogDir)"
Write-Host ""
Write-Host "现在就想让它跑起来:" -ForegroundColor Cyan
if ($OnDemand) {
    Write-Host "  D:\esp\onegpio\tools\start_launcher.ps1"
    Write-Host "  (重启 PC 后什么都不用管: 打开 Scratch 点任意积木, 服务会自己起来)"
} else {
    Write-Host "  D:\esp\onegpio\tools\start_s3extend.ps1 -Background"
}
Write-Host "查看状态 / 停止:"
Write-Host "  D:\esp\onegpio\tools\start_launcher.ps1 -Check"
Write-Host "  D:\esp\onegpio\tools\stop_s3extend.ps1        (停三件套)"
Write-Host "  D:\esp\onegpio\tools\start_launcher.ps1 -Stop (停启动器)"
