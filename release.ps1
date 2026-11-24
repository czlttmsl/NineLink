# Builds NineLink, packages it and publishes a GitHub release that the launcher's auto-updater picks up.
#
#   powershell -File release.ps1 -Version 0.1.1 -Notes "修复了 xxx"
#   powershell -File release.ps1 -Version 0.2.0 -NotesFile notes.md
#   powershell -File release.ps1 -Version 0.1.1 -DryRun     # build + zip only, publish nothing
#
# Release assets (names matter, see src/launcher/Updater.cs):
#   NineLink-<ver>.zip         player files: what the auto-updater installs
#   NineLink-server-<ver>.zip  optional lobby/relay server
#   SHA256SUMS.txt             checksums; the updater refuses a zip that does not match
param(
    [Parameter(Mandatory)] [string]$Version,
    [string]$Notes = "",
    [string]$NotesFile = "",
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root
$env:Path = "C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI;" + $env:Path

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 1.2.3 (got '$Version')" }
$tag = "v$Version"

if (-not $DryRun) {
    if (git status --porcelain) { throw 'There are uncommitted changes. Commit them first.' }
    if ((git rev-parse --abbrev-ref HEAD) -ne 'main') { throw 'Releases are made from the main branch.' }
    if (git tag --list $tag) { throw "Tag $tag already exists." }
}

# 1. version number (read by the launcher, compared with the release tag)
$props = Join-Path $root 'Directory.Build.props'
$propsBefore = Get-Content $props -Raw
$propsBefore -replace '<Version>[^<]*</Version>', "<Version>$Version</Version>" |
    Set-Content $props -Encoding utf8 -NoNewline

# 2. build
& (Join-Path $root 'build.ps1')

# 3. package
$dist = Join-Path $root 'dist'
$out  = Join-Path $root "out\release\$tag"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$stage = Join-Path $out 'NineLink'
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in 'NineLink.exe','ninelink_loader.exe','ninelink.dll','game.ver','empty_kingdom.json') {
    Copy-Item (Join-Path $dist $f) $stage
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip    = Join-Path $out "NineLink-$Version.zip"
$srvZip = Join-Path $out "NineLink-server-$Version.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
[IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $dist 'server'), $srvZip)

$sums = Join-Path $out 'SHA256SUMS.txt'
($zip, $srvZip | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_ -Algorithm SHA256).Hash.ToLower(), (Split-Path $_ -Leaf)
}) -join "`n" | Set-Content $sums -Encoding ascii -NoNewline
Write-Host "packaged -> $out"
Get-ChildItem $out -File | Format-Table Name, Length -AutoSize

if ($DryRun) {
    Set-Content $props $propsBefore -Encoding utf8 -NoNewline
    Write-Host 'Dry run: nothing committed or published.'
    return
}

# 4. commit version, tag, publish
git add $props
git commit -q -m "Release $tag"
git tag $tag
git push -q origin main $tag

$notesArgs = if ($NotesFile) { @('--notes-file', $NotesFile) } elseif ($Notes) { @('--notes', $Notes) } else { @('--generate-notes') }
gh release create $tag $zip $srvZip $sums --title "NineLink $tag" @notesArgs
Write-Host "published $tag -> https://github.com/czlttmsl/NineLink/releases/tag/$tag"
