# Descarrega o FFmpeg pré-compilado (LGPL, shared) para third_party/ffmpeg
# Uso (PowerShell, na raiz do repositório):  ./scripts/get-deps.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party/ffmpeg"
if (Test-Path (Join-Path $dest "include/libavcodec/avcodec.h")) { Write-Host "FFmpeg já existe em $dest"; exit 0 }

$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl-shared.zip"
$tmp = Join-Path $env:TEMP "ffmpeg-playerslice.zip"
Write-Host "A descarregar $url"
Invoke-WebRequest -Uri $url -OutFile $tmp
$ex = Join-Path $env:TEMP "ffmpeg-playerslice"
if (Test-Path $ex) { Remove-Item -Recurse -Force $ex }
Expand-Archive $tmp -DestinationPath $ex
$inner = Get-ChildItem $ex -Directory | Select-Object -First 1
New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item $inner.FullName $dest
Write-Host "FFmpeg pronto em $dest"
