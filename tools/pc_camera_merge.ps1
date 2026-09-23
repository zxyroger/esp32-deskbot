<#
.SYNOPSIS
    多帧平均压横纹: 用装了 Pillow 的 ESP-IDF Python 跑 pc_camera_merge.py
.EXAMPLE
    .\pc_camera_merge.ps1 192.168.0.102                 # 拍 12 帧合并成 merged.png
    .\pc_camera_merge.ps1 192.168.0.102 24 big.png      # 拍 24 帧 (更干净)
#>
param(
    [Parameter(Mandatory = $true)][string]$Board,
    [int]$Frames = 12,
    [string]$Out = "merged.png"
)

$ErrorActionPreference = 'Stop'
$py = 'C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    throw "找不到 ESP-IDF 的 Python: $py"
}

& $py (Join-Path $PSScriptRoot 'pc_camera_merge.py') --host $Board --frames $Frames --out $Out
exit $LASTEXITCODE
