# Builds the player package from the current build: dist\GGDPVR-<version>\ and dist\GGDPVR-<version>.zip
#   GGDPVR-<version>\
#     GGDPVR-Setup.exe (the setup program - the mod's files are inside it), README.txt
#     Binaries\Win64\dinput8.dll, Binaries\Win64\GGDPVR\GGDPVR.dll + ggdpvr.ini   (manual install: mirrors the game folder)
param([string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $root "out\$Config"
$m = Select-String -Path (Join-Path $root 'src\dllmain.cpp') -Pattern '#define GGDPVR_VERSION "([0-9.]+)' | Select-Object -First 1
if (-not $m) { throw 'version not found in dllmain.cpp' }
$ver = $m.Matches[0].Groups[1].Value
$dist = Join-Path $root 'dist'
$pkg = Join-Path $dist "GGDPVR-$ver"
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force "$pkg\Binaries\Win64\GGDPVR" | Out-Null

function Write-Crlf([string]$from, [string]$to, [hashtable]$replace = @{}) {
    # text files go out with Windows line endings, UTF-8 without BOM (ASCII in practice)
    $t = [IO.File]::ReadAllText($from) -replace "`r`n", "`n"
    foreach ($k in $replace.Keys) { $t = $t.Replace($k, $replace[$k]) }
    [IO.File]::WriteAllText($to, ($t -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
}
# the setup must carry the same mod files as the manual-install folder: it is built after them
$setup = Get-Item "$out\GGDPVR-Setup.exe"
foreach ($f in "$out\dinput8.dll", "$out\GGDPVR.dll", "$root\config\ggdpvr.ini") {
    if ((Get-Item $f).LastWriteTime -gt $setup.LastWriteTime) { throw "$f is newer than GGDPVR-Setup.exe - run build.bat" }
}
Copy-Item $setup.FullName "$pkg\GGDPVR-Setup.exe"
Copy-Item "$out\dinput8.dll" "$pkg\Binaries\Win64\dinput8.dll"
Copy-Item "$out\GGDPVR.dll" "$pkg\Binaries\Win64\GGDPVR\GGDPVR.dll"
Write-Crlf "$root\config\ggdpvr.ini" "$pkg\Binaries\Win64\GGDPVR\ggdpvr.ini"
Write-Crlf "$PSScriptRoot\README.txt" "$pkg\README.txt" @{ '{VERSION}' = $ver }

# plain text for any editor: ASCII only
foreach ($f in "$pkg\README.txt", "$pkg\Binaries\Win64\GGDPVR\ggdpvr.ini") {
    $bad = [IO.File]::ReadAllBytes($f) | Where-Object { $_ -gt 127 } | Select-Object -First 1
    if ($null -ne $bad) { throw "non-ASCII character in $f" }
}
# zip entries store local times: all files dated 00:00 of the (UTC) build day, so the package carries no time of day
$stamp = [datetime]::ParseExact([datetime]::UtcNow.ToString('yyyy-MM-dd'), 'yyyy-MM-dd', $null)
Get-ChildItem $pkg -Recurse | ForEach-Object { $_.LastWriteTime = $stamp }
$zip = "$pkg.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip
Get-ChildItem $pkg -Recurse -File | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($pkg.Length + 1) }
"package: $zip ({0:N0} bytes)" -f (Get-Item $zip).Length
