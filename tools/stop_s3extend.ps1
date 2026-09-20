<#
.SYNOPSIS
    停止 s3-extend 的全部后台组件 (backplane / wsgw / esp32gw / s32)

.EXAMPLE
    .\stop_s3extend.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'SilentlyContinue'

$names = 's32', 'backplane', 'wsgw', 'esp32gw'
$stopped = @()

# 先停守护进程 (按 PID 文件), 否则它会把组件重新拉起来
$logDir = Join-Path $env:LOCALAPPDATA 's3extend\logs'
$pidFile = Join-Path $logDir 'supervisor.pid'
if (Test-Path $pidFile) {
    $supervisorPid = (Get-Content -LiteralPath $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($supervisorPid) {
        $proc = Get-Process -Id ([int]$supervisorPid) -ErrorAction SilentlyContinue
        if ($proc -and $proc.ProcessName -eq 'powershell') {
            $stopped += "守护进程 (PID $supervisorPid)"
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 500

foreach ($name in $names) {
    foreach ($proc in Get-Process $name -ErrorAction SilentlyContinue) {
        $stopped += "$name (PID $($proc.Id))"
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
}

# 兜底: 某些情况下真正的监听进程是 python.exe
foreach ($proc in Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue) {
    if ($proc.CommandLine -match 'backplane|wsgw|esp32gw|s3_extend') {
        $stopped += "python (PID $($proc.ProcessId))"
        Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

Start-Sleep -Seconds 2

$left = Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
    Where-Object { $_.LocalPort -in 9007, 43124, 43125 }

if ($stopped.Count -eq 0) {
    Write-Host "没有正在运行的 s3-extend 组件。" -ForegroundColor Yellow
} else {
    Write-Host ("已停止: " + ($stopped -join ', ')) -ForegroundColor Green
}

if ($left) {
    Write-Host "注意: 以下端口仍被占用:" -ForegroundColor Red
    $left | Select-Object LocalAddress, LocalPort, OwningProcess | Format-Table -AutoSize
} else {
    Write-Host "9007 / 43124 / 43125 均已释放。" -ForegroundColor Green
}
