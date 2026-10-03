<#
.SYNOPSIS
    修 onegpio 摄像头"每 ~6 秒卡一下 / 连接不稳"的电脑端病根: 无线网卡周期性重关联。

.DESCRIPTION
    视频卡顿的根因常常不在板子, 而在这台电脑的无线网卡: 它每几秒把安全关联拆了
    重建一次, 期间链路整段丢包, 视频就定格一秒左右, 网关到板子的 TCP 也会跟着断。
    典型证据是 WLAN-AutoConfig 事件日志里 11004/11010/11005 每 6 秒一组, 而且
    "ping 路由器" 和 "ping 板子" 会在同一时刻一起超时。

    本脚本改四样东西 (默认改完自动重启网卡):

        SupportMACRandom      MAC Randomization      1 -> 0
        *WakeOnMagicPacket    Wake on Magic Packet   1 -> 0
        *WakeOnPattern        Wake on Pattern Match  1 -> 0
        WirelessMode          无线模式               8(自动) -> 64(仅 5GHz)

    后两类是 WoWLAN / 漫游扫描相关的开关; 锁 5GHz 是为了不被同名 2.4G/5G 的
    频段引导"两边拉锯"。不想锁 5GHz 就加 -KeepAutoBand。

    用法:
      .\fix_wlan_stability.ps1                 # 改 + 重启网卡 + 复查 (会弹 UAC)
      .\fix_wlan_stability.ps1 -Check          # 只看现状, 不改 (不需要管理员)
      .\fix_wlan_stability.ps1 -KeepAutoBand   # 只关随机化/唤醒, 不动无线模式
      .\fix_wlan_stability.ps1 -NoRestart      # 改完不自动重启网卡
      .\fix_wlan_stability.ps1 -Revert         # 还原成上一次修改前的值
      .\fix_wlan_stability.ps1 -Adapter WLAN   # 指定网卡名 (默认自动找无线网卡)

    改之前的原值会存到
    %LOCALAPPDATA%\s3extend\logs\wlan_settings_backup.json, 供 -Revert 还原。

    注意: 这种重关联是**阵发性**的 —— 环境一安静它就自己停了,
    所以 -Check 里除了"近 N 分钟"还会给一个"近 30 分钟"的基数,
    免得刚好赶上安静期, 把偶发当成没问题。

.EXAMPLE
    .\fix_wlan_stability.ps1 -Check

.EXAMPLE
    .\fix_wlan_stability.ps1

.EXAMPLE
    .\fix_wlan_stability.ps1 -Revert
#>
[CmdletBinding()]
param(
    [string]$Adapter = '',
    [switch]$Check,
    [switch]$Revert,
    [switch]$KeepAutoBand,
    [switch]$NoRestart,
    [switch]$NoElevate,
    [int]$VerifyMinutes = 3
)

$ErrorActionPreference = 'Stop'

$logDir     = Join-Path $env:LOCALAPPDATA 's3extend\logs'
$backupFile = Join-Path $logDir 'wlan_settings_backup.json'

# 要改的属性。RegistryValue 是十六进制/十进制数字, 与系统语言无关。
$settings = @(
    [pscustomobject]@{ Keyword = 'SupportMACRandom';   Value = 0;  Label = 'MAC Randomization';     Desc = '关掉随机 MAC, 少一次诱发重关联的因素' },
    [pscustomobject]@{ Keyword = '*WakeOnMagicPacket'; Value = 0;  Label = 'Wake on Magic Packet';  Desc = '关掉 WoWLAN 魔法包唤醒' },
    [pscustomobject]@{ Keyword = '*WakeOnPattern';     Value = 0;  Label = 'Wake on Pattern Match'; Desc = '关掉 WoWLAN 模式匹配唤醒 (会周期性扫描)' }
)
if (-not $KeepAutoBand) {
    $settings += [pscustomobject]@{ Keyword = 'WirelessMode'; Value = 64; Label = '无线模式 (仅 5GHz)'; Desc = '不再被同名 2.4G/5G 频段引导来回拉' }
}

# -Revert 找不到备份文件时的兜底值 (芯片出厂默认)
$revertDefaults = @{
    'SupportMACRandom'   = 1
    '*WakeOnMagicPacket' = 1
    '*WakeOnPattern'     = 1
    'WirelessMode'       = 8
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-WlanAdapter {
    param([string]$Name)

    if ($Name) {
        $found = Get-NetAdapter -Name $Name -ErrorAction SilentlyContinue
        if (-not $found) { throw "找不到网卡 '$Name'。可用: $((Get-NetAdapter | Select-Object -ExpandProperty Name) -join ', ')" }
        return $found
    }

    $candidates = @(Get-NetAdapter -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceDescription -match 'Wireless|WLAN|Wi-Fi|802\.11' })
    if ($candidates.Count -eq 0) { throw "找不到无线网卡, 请用 -Adapter 指定名字。" }

    $up = @($candidates | Where-Object { $_.Status -eq 'Up' })
    if ($up.Count -gt 0) { return $up[0] }
    return $candidates[0]
}

function Get-PropRow {
    param([string]$AdapterName, [string]$Keyword)
    return Get-NetAdapterAdvancedProperty -Name $AdapterName -RegistryKeyword $Keyword -ErrorAction SilentlyContinue |
        Select-Object -First 1
}

function Get-PropValue {
    param([string]$AdapterName, [string]$Keyword)
    $row = Get-PropRow -AdapterName $AdapterName -Keyword $Keyword
    if (-not $row) { return $null }
    return ($row.RegistryValue | Select-Object -First 1)
}

function Get-ReassocCount {
    # 11004 = "无线安全功能已停止", 重关联的第一步; 每重关联一次一条
    param([datetime]$Since)
    try {
        $events = Get-WinEvent -FilterHashtable @{
            LogName   = 'Microsoft-Windows-WLAN-AutoConfig/Operational'
            StartTime = $Since
            Id        = 11004
        } -ErrorAction Stop
        return @($events).Count
    } catch {
        # 这段时间一条都没有时 Get-WinEvent 也会抛异常, 那是正常的 0;
        # 其它错误 (比如读不到日志) 返回 -1, 免得把"读不到"当成"健康"。
        if ($_.Exception.Message -match 'No events were found|找不到') { return 0 }
        Write-Host ("  读 WLAN 事件日志失败: " + $_.Exception.Message) -ForegroundColor Red
        return -1
    }
}

function Test-LinkQuality {
    param([string]$Target, [int]$Count = 20)

    $ping = New-Object System.Net.NetworkInformation.Ping
    $ok = 0; $lost = 0; $max = 0; $sum = 0
    for ($i = 0; $i -lt $Count; $i++) {
        try {
            $reply = $ping.Send($Target, 800)
            if ($reply.Status -eq 'Success') {
                $ok++; $sum += $reply.RoundtripTime
                if ($reply.RoundtripTime -gt $max) { $max = $reply.RoundtripTime }
            } else {
                $lost++
            }
        } catch {
            $lost++
        }
        Start-Sleep -Milliseconds 150
    }
    $avg = 0
    if ($ok -gt 0) { $avg = [math]::Round($sum / $ok, 1) }
    return [pscustomobject]@{ Target = $Target; Sent = $Count; Lost = $lost; Avg = $avg; Max = $max }
}

function Show-CurrentState {
    param($WlanAdapter, [int]$Minutes)

    Write-Host ""
    Write-Host "网卡: $($WlanAdapter.Name)  ($($WlanAdapter.InterfaceDescription))" -ForegroundColor Cyan
    Write-Host "状态: $($WlanAdapter.Status)   速率: $($WlanAdapter.LinkSpeed)"

    $iface = netsh wlan show interfaces 2>$null
    foreach ($key in 'SSID', 'BSSID', 'Radio type', 'Channel', 'Signal') {
        $line = $iface | Select-String -Pattern "^\s*$key\s+:" | Select-Object -First 1
        if ($line) { Write-Host ("  " + $line.Line.Trim()) }
    }

    Write-Host ""
    Write-Host "当前相关属性:" -ForegroundColor Cyan
    foreach ($s in $settings) {
        $row = Get-PropRow -AdapterName $WlanAdapter.Name -Keyword $s.Keyword
        if (-not $row) {
            Write-Host ("  {0,-26} (这块网卡没有这个属性)" -f $s.Label) -ForegroundColor DarkGray
        } else {
            $color = 'Yellow'
            if ("$($row.RegistryValue | Select-Object -First 1)" -eq "$($s.Value)") { $color = 'Green' }
            Write-Host ("  {0,-26} = {1}" -f $s.Label, $row.DisplayValue) -ForegroundColor $color
        }
    }

    # 顺带看一眼无线模式: 不锁 5GHz 时它是"自动", 这里也报出来
    if ($KeepAutoBand) {
        $row = Get-PropRow -AdapterName $WlanAdapter.Name -Keyword 'WirelessMode'
        if ($row) { Write-Host ("  {0,-26} = {1} (本次保持不动)" -f '无线模式', $row.DisplayValue) -ForegroundColor DarkGray }
    }

    $since = (Get-Date).AddMinutes(-$Minutes)
    $count = Get-ReassocCount -Since $since
    $count30 = Get-ReassocCount -Since (Get-Date).AddMinutes(-30)
    Write-Host ""
    Write-Host ("近 {0} 分钟 WLAN 重关联 (事件 11004): {1} 次" -f $Minutes, $count) -ForegroundColor Cyan
    Write-Host ("近 30 分钟: {0} 次" -f $count30) -ForegroundColor Cyan
    if ($count -eq -1 -or $count30 -eq -1) {
        Write-Host "  -> 读不到事件日志, 请用管理员身份运行再看。" -ForegroundColor Red
    } elseif ($count30 -ge 120) {
        Write-Host "  -> 偏多, 基本就是「每几秒重关联一次」, 视频会周期性定格。" -ForegroundColor Yellow
    } elseif ($count30 -eq 0) {
        Write-Host "  -> 最近 30 分钟没有重关联, 健康。" -ForegroundColor Green
    } else {
        Write-Host "  -> 偶发重关联。这种毛病可能是阵发的: 卡的时候再跑一次 -Check 对比。" -ForegroundColor DarkGray
    }

    $gw = (Get-NetIPConfiguration -InterfaceAlias $WlanAdapter.Name -ErrorAction SilentlyContinue).IPv4DefaultGateway.NextHop
    if ($gw) {
        Write-Host ""
        Write-Host "探测网关 $gw (20 个包)..." -ForegroundColor Cyan
        $q = Test-LinkQuality -Target $gw -Count 20
        Write-Host ("  丢包 {0}/{1}, 平均 {2}ms, 最大 {3}ms" -f $q.Lost, $q.Sent, $q.Avg, $q.Max) -ForegroundColor Cyan
    }
    Write-Host ""
}

function Save-Backup {
    param($WlanAdapter, $Rows)

    if (-not (Test-Path -LiteralPath $logDir)) {
        New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    }

    $values = @{}
    foreach ($key in $Rows.Keys) { $values[$key] = $Rows[$key] }

    $payload = [pscustomobject]@{
        adapter    = $WlanAdapter.Name
        instanceId = $WlanAdapter.InterfaceGuid
        savedAt    = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
        values     = $values
    }
    $payload | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $backupFile -Encoding UTF8
    Write-Host "已保存改前原值: $backupFile" -ForegroundColor DarkGray
}

function Restart-Wlan {
    param($WlanAdapter)

    Write-Host ""
    Write-Host "重启网卡 $($WlanAdapter.Name) (会断开重连几秒)..." -ForegroundColor Cyan
    try {
        Restart-NetAdapter -Name $WlanAdapter.Name -Confirm:$false
    } catch {
        Write-Host "重启网卡失败: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "可以手动执行: Restart-NetAdapter -Name $($WlanAdapter.Name)" -ForegroundColor Yellow
        return $false
    }

    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 1
        $now = Get-NetAdapter -Name $WlanAdapter.Name -ErrorAction SilentlyContinue
        if ($now -and $now.Status -eq 'Up') {
            Write-Host "网卡已恢复 ($($now.Status), $($now.LinkSpeed))。" -ForegroundColor Green
            return $true
        }
    }
    Write-Host "网卡 30 秒内还没起来, 请检查 WiFi 连接。" -ForegroundColor Yellow
    return $false
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
$isAdmin = Test-IsAdmin

# -Check 只读, 不需要管理员; 改/还原需要, 不是就自己弹 UAC 重新拉起
if ((-not $isAdmin) -and (-not $Check) -and (-not $NoElevate)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', "`"$PSCommandPath`"")
    if ($Adapter)     { $argList += @('-Adapter', "`"$Adapter`"") }
    if ($Revert)      { $argList += '-Revert' }
    if ($KeepAutoBand){ $argList += '-KeepAutoBand' }
    if ($NoRestart)   { $argList += '-NoRestart' }
    $argList += '-NoElevate'
    if ($VerifyMinutes -ne 3) { $argList += @('-VerifyMinutes', $VerifyMinutes) }

    Write-Host "需要管理员权限改网卡属性, 正在弹出 UAC..." -ForegroundColor Yellow
    Write-Host "(请在弹出的窗口里点「是」; 改网卡的输出在那个新窗口里)" -ForegroundColor DarkGray
    try {
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argList
    } catch {
        Write-Host "提权失败: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "请手动开一个【以管理员身份运行】的 PowerShell, 再跑一次本脚本。" -ForegroundColor Yellow
        exit 1
    }
    exit 0
}

$wlan = Resolve-WlanAdapter -Name $Adapter

if ($Check) {
    Show-CurrentState -WlanAdapter $wlan -Minutes $VerifyMinutes
    exit 0
}

if (-not $isAdmin) {
    Write-Host "需要管理员权限 (改网卡属性), 请用【以管理员身份运行】的 PowerShell 再试。" -ForegroundColor Red
    exit 1
}

if ($Revert) {
    Write-Host "还原网卡设置..." -ForegroundColor Cyan

    $restore = $null
    if (Test-Path -LiteralPath $backupFile) {
        try {
            $restore = (Get-Content -LiteralPath $backupFile -Encoding UTF8 -Raw | ConvertFrom-Json).values
            Write-Host "用备份文件: $backupFile" -ForegroundColor DarkGray
        } catch {
            Write-Host "备份文件读不出来, 改用出厂默认值。" -ForegroundColor Yellow
        }
    } else {
        Write-Host "没有备份文件, 改用出厂默认值 (1/1/1/自动)。" -ForegroundColor Yellow
    }

    $changed = 0
    foreach ($key in $revertDefaults.Keys) {
        $row = Get-PropRow -AdapterName $wlan.Name -Keyword $key
        if (-not $row) { continue }

        $target = $revertDefaults[$key]
        if ($restore -and ($restore.PSObject.Properties.Name -contains $key)) {
            $target = [int]$restore.PSObject.Properties[$key].Value
        }
        if ("$($row.RegistryValue | Select-Object -First 1)" -eq "$target") {
            Write-Host "  $($row.DisplayName) 已经是 $($row.DisplayValue), 跳过" -ForegroundColor DarkGray
            continue
        }
        try {
            Set-NetAdapterAdvancedProperty -Name $wlan.Name -RegistryKeyword $key -RegistryValue "$target" -NoRestart
            $after = Get-PropRow -AdapterName $wlan.Name -Keyword $key
            Write-Host "  $($row.DisplayName): $($row.DisplayValue) -> $($after.DisplayValue)" -ForegroundColor Green
            $changed++
        } catch {
            Write-Host "  还原 $($row.DisplayName) 失败: $($_.Exception.Message)" -ForegroundColor Red
        }
    }

    if ($changed -gt 0 -and -not $NoRestart) { Restart-Wlan -WlanAdapter $wlan | Out-Null }
    Write-Host "还原完成。" -ForegroundColor Green
    if (-not $NoRestart) { Show-CurrentState -WlanAdapter $wlan -Minutes $VerifyMinutes }
    exit 0
}

# ---- 应用 ----------------------------------------------------------------
Write-Host ""
Write-Host "改之前的状态:" -ForegroundColor Cyan
$beforeCount = Get-ReassocCount -Since (Get-Date).AddMinutes(-$VerifyMinutes)
Write-Host ("  近 {0} 分钟 WLAN 重关联: {1} 次" -f $VerifyMinutes, $beforeCount) -ForegroundColor Cyan

# 存一份原值, 方便 -Revert
$original = @{}
foreach ($key in $revertDefaults.Keys) {
    $v = Get-PropValue -AdapterName $wlan.Name -Keyword $key
    if ($null -ne $v) { $original[$key] = [int]$v }
}
Save-Backup -WlanAdapter $wlan -Rows $original

Write-Host ""
Write-Host "应用设置:" -ForegroundColor Cyan
$changed = 0
foreach ($s in $settings) {
    $row = Get-PropRow -AdapterName $wlan.Name -Keyword $s.Keyword
    if (-not $row) {
        Write-Host ("  {0,-26} 跳过 (这块网卡没有这个属性)" -f $s.Label) -ForegroundColor DarkGray
        continue
    }
    if ("$($row.RegistryValue | Select-Object -First 1)" -eq "$($s.Value)") {
        Write-Host ("  {0,-26} 已经是 {1}, 不用改" -f $s.Label, $row.DisplayValue) -ForegroundColor DarkGray
        continue
    }
    try {
        Set-NetAdapterAdvancedProperty -Name $wlan.Name -RegistryKeyword $s.Keyword -RegistryValue "$($s.Value)" -NoRestart
        $after = Get-PropRow -AdapterName $wlan.Name -Keyword $s.Keyword
        Write-Host ("  {0,-26} {1} -> {2}   ({3})" -f $s.Label, $row.DisplayValue, $after.DisplayValue, $s.Desc) -ForegroundColor Green
        $changed++
    } catch {
        Write-Host ("  {0,-26} 改失败: {1}" -f $s.Label, $_.Exception.Message) -ForegroundColor Red
    }
}

if ($changed -eq 0) {
    Write-Host ""
    Write-Host "没有需要改的项 —— 这些设置已经是目标值了。" -ForegroundColor Yellow
}

if (-not $NoRestart) {
    $applyTime = Get-Date
    Restart-Wlan -WlanAdapter $wlan | Out-Null

    # 重启后静置 60 秒, 再看这段时间里重关联几次
    Write-Host ""
    Write-Host "静置 60 秒后复查重关联频率..." -ForegroundColor Cyan
    Start-Sleep -Seconds 60
    $afterCount = Get-ReassocCount -Since $applyTime
    $perMinBefore = 0
    if ($VerifyMinutes -gt 0) { $perMinBefore = [math]::Round($beforeCount / $VerifyMinutes, 1) }

    Write-Host ""
    Write-Host "==== 结果 ====" -ForegroundColor Cyan
    Write-Host ("  改之前: 近 {0} 分钟 {1} 次 (约 {2} 次/分钟)" -f $VerifyMinutes, $beforeCount, $perMinBefore)
    Write-Host ("  改之后: 重启后 60 秒内 {0} 次" -f $afterCount) -ForegroundColor $(if ($afterCount -eq 0) { 'Green' } else { 'Yellow' })
    if ($afterCount -eq 0) {
        Write-Host "  -> 重关联停了。再确认视频: python tools\sniff_camera_stream.py --via ws --seconds 25" -ForegroundColor Green
    } else {
        Write-Host "  -> 还有重关联。下一步: 更新 8821CE 驱动, 或给电脑插网线。" -ForegroundColor Yellow
        Write-Host "     驱动更新: 设置 -> Windows 更新 -> 高级选项 -> 可选更新 -> 驱动程序更新" -ForegroundColor DarkGray
    }

    $gw = (Get-NetIPConfiguration -InterfaceAlias $wlan.Name -ErrorAction SilentlyContinue).IPv4DefaultGateway.NextHop
    if ($gw) {
        Write-Host ""
        Write-Host "探测网关 $gw (30 个包)..." -ForegroundColor Cyan
        $q = Test-LinkQuality -Target $gw -Count 30
        Write-Host ("  丢包 {0}/{1}, 平均 {2}ms, 最大 {3}ms" -f $q.Lost, $q.Sent, $q.Avg, $q.Max) -ForegroundColor Cyan
    }
    Write-Host ""
    Write-Host "想还原: .\fix_wlan_stability.ps1 -Revert" -ForegroundColor DarkGray
    Write-Host ""
} else {
    Write-Host ""
    Write-Host "已改完但没有重启网卡。手动生效: Restart-NetAdapter -Name $($wlan.Name)" -ForegroundColor Yellow
    Write-Host "想还原: .\fix_wlan_stability.ps1 -Revert" -ForegroundColor DarkGray
}
