# Removes everything GGDPVR installed (only files listed in installed_manifest.tsv that are ours),
# then verifies Binaries\Win64 top-level files match the pre-install record. Nothing else is touched.
$ErrorActionPreference = 'Stop'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\GalGun Double Peace\Binaries\Win64'
$manifest = Join-Path $PSScriptRoot 'installed_manifest.tsv'
if (-not (Test-Path $manifest)) { "nothing installed"; return }
$paths = Get-Content $manifest | Select-Object -Skip 1 | ForEach-Object { ($_ -split "`t")[0] } | Sort-Object -Unique
foreach ($p in $paths) {
    if (Test-Path $p) { Remove-Item $p -Force; "removed $p" }
}
# the GGDPVR folder: removed when empty
foreach ($name in @('GGDPVR')) {
    $dir = Join-Path $game $name
    if ((Test-Path $dir) -and -not (Get-ChildItem $dir -Force)) { Remove-Item $dir; "removed empty $dir" }
    elseif (Test-Path $dir) { "left $dir (contains files not installed by us)" }
}
# verify
$pre = Import-Csv (Join-Path $PSScriptRoot 'preinstall_win64_files.tsv') -Delimiter "`t" -Header name,len,sha
$now = Get-ChildItem $game -File | Select-Object -ExpandProperty Name
$extra = $now | Where-Object { $_ -notin $pre.name }
$missing = $pre.name | Where-Object { $_ -notin $now }
"extra top-level files vs pre-install: $($extra -join ', ')"
"missing top-level files vs pre-install: $($missing -join ', ')"
