# Builds everything into dist\ :  NineLink.exe, ninelink_loader.exe, ninelink.dll, game.ver
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
$obj  = Join-Path $root 'build\mod'
New-Item -ItemType Directory -Force $dist, $obj | Out-Null

# --- ninelink.dll (MSVC) ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found (install VS 2022 Build Tools with the C++ workload)' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$src = Join-Path $root 'src\mod\*.cpp'
$cl = "cl /nologo /std:c++20 /O2 /EHa /MT /LD /utf-8 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX " +
      "`"$src`" /Fo`"$obj\\`" /Fe`"$dist\ninelink.dll`" /link /NOLOGO user32.lib gdi32.lib ws2_32.lib"
cmd /c "`"$vcvars`" >nul 2>&1 && $cl"
if ($LASTEXITCODE -ne 0) { throw "ninelink.dll build failed ($LASTEXITCODE)" }
Remove-Item "$dist\ninelink.exp","$dist\ninelink.lib" -ErrorAction SilentlyContinue

# --- loader + launcher (.NET 8) ---
foreach ($p in 'loader\Loader.csproj','launcher\Launcher.csproj') {
    dotnet publish (Join-Path $root "src\$p") -c Release -o $dist --nologo -v quiet
    if ($LASTEXITCODE -ne 0) { throw "$p publish failed" }
}
Get-ChildItem $dist -Filter *.pdb | Remove-Item

# --- lobby/relay server (.NET 8, framework-dependent; runs on Windows and Linux) ---
$srv = Join-Path $dist 'server'
dotnet publish (Join-Path $root 'src\server\Server.csproj') -c Release -o $srv --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'server publish failed' }
Get-ChildItem $srv -Filter *.pdb | Remove-Item

Copy-Item (Join-Path $root 'game.ver') $dist -Force
# Opponent board shown before a friend's first real snapshot arrives (a bare castle).
Copy-Item (Join-Path $root 'src\mod\data\empty_kingdom.json') $dist -Force
Write-Host "built -> $dist"
Get-ChildItem $dist -File | Format-Table Name, Length -AutoSize
