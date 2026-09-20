<#
.SYNOPSIS
    在本机一键使用 ESP-IDF v5.5.4 构建 / 烧录 / 监控本工程。

.EXAMPLE
    .\idf.ps1 build
    .\idf.ps1 menuconfig
    .\idf.ps1 -Port COM5 flash monitor
    .\idf.ps1 -Port COM5 monitor
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Port,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Action = @('build')
)

$ErrorActionPreference = 'Stop'
Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process -Force

$projectDir = Split-Path -Parent $PSScriptRoot
$firmwareDir = Join-Path $projectDir 'firmware'

# 优先使用 ESP-IDF 安装管理器生成的 profile，其次退回官方 export.ps1
$profiles = @(
    'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1',
    (Join-Path $env:USERPROFILE '.espressif\Microsoft.v5.5.4.PowerShell_profile.ps1')
)
$profileScript = $profiles | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($profileScript) {
    Write-Host "使用 IDF 环境: $profileScript" -ForegroundColor Cyan
    . $profileScript | Out-Null
} else {
    $idfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { 'D:\esp\v5.5.4\v5.5.4\esp-idf' }
    if (-not (Test-Path (Join-Path $idfPath 'export.ps1'))) {
        throw "找不到 ESP-IDF: $idfPath （请设置 IDF_PATH 或修改本脚本）"
    }
    Write-Host "使用 IDF 环境: $idfPath\export.ps1" -ForegroundColor Cyan
    . (Join-Path $idfPath 'export.ps1') | Out-Null
}

Push-Location $firmwareDir
try {
    $arguments = @()
    if ($Port) { $arguments += @('-p', $Port) }
    $arguments += $Action

    Write-Host "idf.py $($arguments -join ' ')" -ForegroundColor Cyan
    & idf.py @arguments
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
