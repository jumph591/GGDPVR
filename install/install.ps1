param([string]$Config = 'RelWithDebInfo', [switch]$KeepIni)
# Installs the GGDPVR test build into the game's Binaries\Win64.
#  - refuses to overwrite a foreign dinput8.dll; replaces only our own earlier files
#  - first install snapshots Binaries\Win64 top-level files and records a manifest
#  - every file written is appended to install\installed_manifest.tsv (path, sha256, time)
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\GalGun Double Peace\Binaries\Win64'
$root = Split-Path $PSScriptRoot
$out  = Join-Path $root "out\$Config"
$manifest = Join-Path $PSScriptRoot 'installed_manifest.tsv'
# string present in our proxy
$ourMarkers = @('GGDPVR loader')

$dst = Join-Path $game 'dinput8.dll'
if (Test-Path $dst) {
    $isOurs = $false
    foreach ($m in $ourMarkers) { if (Select-String -Path $dst -Pattern $m -Encoding Unicode -Quiet) { $isOurs = $true } }
    if (-not $isOurs) { throw "A foreign dinput8.dll exists in $game - refusing to overwrite." }
}
if (-not (Test-Path $manifest)) {
    # First install: record the pre-install state of Binaries\Win64 (top level)
    Get-ChildItem $game -File | ForEach-Object {
        "{0}`t{1}`t{2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash
    } | Set-Content (Join-Path $PSScriptRoot 'preinstall_win64_files.tsv') -Encoding utf8
    "path`tsha256`tinstalled_at" | Set-Content $manifest -Encoding utf8
}
$mod = Join-Path $game 'GGDPVR'
New-Item -ItemType Directory -Force $mod | Out-Null

$files = @(
    @{ src = "$out\dinput8.dll"; dst = $dst },
    @{ src = "$out\GGDPVR.dll";  dst = "$mod\GGDPVR.dll" },
    @{ src = "$out\GGDPVR.pdb";  dst = "$mod\GGDPVR.pdb" }
)
# 0.9.104: one settings file - the separate option reference is retired
if (Test-Path "$mod\ggdpvr_debug.ini") { Remove-Item "$mod\ggdpvr_debug.ini" -Force; "removed retired $mod\ggdpvr_debug.ini" }
if (-not ($KeepIni -and (Test-Path "$mod\ggdpvr.ini"))) {
    $files += @{ src = "$root\config\ggdpvr.ini"; dst = "$mod\ggdpvr.ini" }
}
foreach ($f in $files) {
    Copy-Item $f.src $f.dst -Force
    "{0}`t{1}`t{2}" -f $f.dst, (Get-FileHash $f.dst -Algorithm SHA256).Hash, (Get-Date -Format s) | Add-Content $manifest -Encoding utf8
    "installed $($f.dst)"
}
