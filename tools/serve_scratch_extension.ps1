<#
.SYNOPSIS
    用本地 HTTP 服务把 Scratch 扩展脚本暴露出去, 供编辑器通过 URL 加载。

.DESCRIPTION
    默认把 D:\esp\onegpio\scratch 目录挂在 http://127.0.0.1:8000/ 上,
    扩展脚本地址就是 http://127.0.0.1:8000/esp32s3.js

    用法:
      .\serve_scratch_extension.ps1            # 启动服务 (Ctrl+C 停止)
      .\serve_scratch_extension.ps1 -Check     # 只检查环境和端口

.EXAMPLE
    .\serve_scratch_extension.ps1
#>
[CmdletBinding()]
param(
    [int]$Port = 8000,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'scratch'
$file = Join-Path $root 'esp32s3.js'
$url = "http://127.0.0.1:$Port/esp32s3.js"

if (-not (Test-Path $file)) {
    Write-Host "找不到扩展脚本: $file" -ForegroundColor Red
    exit 1
}

$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) {
    $python = (Get-Command py -ErrorAction SilentlyContinue).Source
}
if (-not $python) {
    Write-Host "找不到 Python, 无法启动本地 HTTP 服务。" -ForegroundColor Red
    Write-Host "也可以手工用任意静态服务器把 $root 目录发布出去。" -ForegroundColor Yellow
    exit 1
}

$busy = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
if ($busy) {
    Write-Host "端口 $Port 已被占用 (PID $($busy.OwningProcess)), 请换一个 -Port。" -ForegroundColor Red
    exit 1
}

Write-Host "扩展脚本目录 : $root" -ForegroundColor Cyan
Write-Host "加载地址     : $url" -ForegroundColor Green
Write-Host "把这个地址填进编辑器的自定义扩展框 (Ctrl+C 停止服务)" -ForegroundColor Yellow
Write-Host "提示: start_launcher.ps1 的启动器也提供同一个地址, 而且点积木时会自动" -ForegroundColor DarkGray
Write-Host "      拉起 s3-extend; 只想托管文件才需要单独跑本脚本。" -ForegroundColor DarkGray

if ($Check) { exit 0 }

& $python -m http.server $Port --bind 127.0.0.1 --directory $root
