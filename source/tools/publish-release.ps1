# Publishes the deployed Wavy-Flo as a release that every copy of Wavy-Flo
# finds through Help > Check for Updates (and on start).
#
#   1. Raise kWavyFloVersion in source/src/updater.h, build and deploy as usual
#      (Wavy-Flo Builder), so the folder holds the new Wavy-Flo.exe.
#   2. Write what is new in RELEASE-NOTES.md beside it.
#   3. Run publish-release.cmd (or this script). -DryRun makes the zip and
#      stops before anything is uploaded.
#   4. Before the release is made, its source goes into source/ of the same
#      repository as one commit (publish-source.ps1), so the release's tag
#      holds it; -NoSource leaves that out.
#
# What goes up: Wavy-Flo-<version>-win64.zip, the program as it runs -- the
# exe, its DLLs and plugin folders, the effects, the example scripts with
# SCRIPTING.md, and the licences -- never the
# source, builds, backups, crash reports or the large optional FFmpeg in tools.
# Wavy-Flo is handed out as this zip, and copies of it update themselves from
# the next one. -WithSetup also makes and uploads Wavy-Flo-<version>-Setup.exe.
param(
  [switch]$DryRun,
  [switch]$WithSetup,
  [switch]$NoSource,
  [string]$Repo = "Kibo4aa/Wavy-Flo-Releases",
  [string]$From = "",  # the program folder to publish; the deployed one by default
  [string]$Out = ""    # where the zip (and Setup) are made; the releases folder by default
)
$ErrorActionPreference = "Stop"
$app = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # ...\Wavy-Flo
$header = Join-Path $app "source\src\updater.h"
$match = Select-String -Path $header -Pattern 'kWavyFloVersion\s*=\s*"([^"]+)"'
if (-not $match) { throw "No kWavyFloVersion in $header" }
$version = $match.Matches[0].Groups[1].Value
$program = if ($From -ne "") { $From } else { $app }
Write-Host "Wavy-Flo $version from $program"

$exe = Join-Path $program "Wavy-Flo.exe"
if (-not (Test-Path $exe)) { throw "No Wavy-Flo.exe in $program" }
# The deployed program must be one that looks for updates here, and this version.
$bytes = [System.IO.File]::ReadAllBytes($exe)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
if (-not $text.Contains($Repo)) {
  throw "This Wavy-Flo.exe ($((Get-Item $exe).LastWriteTime)) looks for updates somewhere else than $Repo. Build and deploy with Wavy-Flo Builder first."
}
if (-not $text.Contains($version)) {
  Write-Warning "Wavy-Flo.exe does not seem to carry version $version. Was it deployed after updater.h was changed?"
}

$gh = "C:\Program Files\GitHub CLI\gh.exe"
if (-not (Test-Path $gh)) { $gh = "gh" }
if (-not $DryRun) {
  # "release not found" on stderr is the answer hoped for, not a failure:
  # Windows PowerShell would stop on it with errors set to stop.
  $ErrorActionPreference = "Continue"
  & $gh release view "v$version" --repo $Repo 2>&1 | Out-Null
  $exists = $LASTEXITCODE -eq 0
  $ErrorActionPreference = "Stop"
  if ($exists) { throw "v$version is already released. Raise kWavyFloVersion in updater.h first." }
}

# The program as it runs, gathered in a folder of its own.
$stage = Join-Path $env:TEMP "wavy-flo-release-$version"
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
$inside = Join-Path $stage "Wavy-Flo"
New-Item -ItemType Directory -Path $inside | Out-Null
Copy-Item $exe $inside
Get-ChildItem -Path $program -Filter *.dll -File | Copy-Item -Destination $inside
foreach ($name in "README.md") { if (Test-Path (Join-Path $program $name)) { Copy-Item (Join-Path $program $name) $inside } }
# The example scripts as the source has them -- never a person's own scripts
# that may sit in the program's Scripts folder -- and how to write them.
$examples = Join-Path $app "source\scripts"
if (Test-Path $examples) { Copy-Item $examples -Destination (Join-Path $inside "Scripts") -Recurse }
$scripting = Join-Path $app "source\SCRIPTING.md"
if (Test-Path $scripting) { Copy-Item $scripting $inside }
foreach ($folder in "effects", "generic", "iconengines", "imageformats", "multimedia", "networkinformation",
                    "platforms", "styles", "tls", "licenses") {
  $from = Join-Path $program $folder
  if (Test-Path $from) { Copy-Item $from -Destination $inside -Recurse }
}
$tools = Join-Path $inside "tools"
New-Item -ItemType Directory -Path $tools | Out-Null
Get-ChildItem -Path (Join-Path $program "tools") -File -ErrorAction SilentlyContinue | Copy-Item -Destination $tools
# What stays on this computer: release-exclude.txt beside the source folder
# lists it, one path per line relative to the program folder (# starts a
# comment). A local build can stage packs that are not ours to hand out.
$exclude = Join-Path $app "release-exclude.txt"
if (Test-Path $exclude) {
  foreach ($line in Get-Content -Encoding UTF8 $exclude) {
    $path = $line.Trim()
    if ($path -eq "" -or $path.StartsWith("#")) { continue }
    $target = Join-Path $inside $path
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force; Write-Host "Left out: $path" }
  }
}

# Wavy-Flo's own programs signed (signing.txt; see sign.ps1) before they are
# packed: the exe, the CUDA plugin, the effects and their editor. Qt's, FFmpeg's
# and Microsoft's files are theirs, and stay as they came.
. (Join-Path $PSScriptRoot "sign.ps1")
$own = @(Join-Path $inside "Wavy-Flo.exe") + @(Get-ChildItem -Path $inside -Filter "Wavy*.dll" -File | ForEach-Object FullName)
$own += @(Get-ChildItem -Path (Join-Path $inside "effects") -File -ErrorAction SilentlyContinue |
          Where-Object { $_.Extension -in ".wfx", ".dll", ".exe" } | ForEach-Object FullName)
$signed = Sign-WavyFlo $own

$out = if ($Out -ne "") { $Out } else { Join-Path $app "releases" }
New-Item -ItemType Directory -Path $out -Force | Out-Null
$zip = Join-Path $out "Wavy-Flo-$version-win64.zip"
Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $inside -DestinationPath $zip -CompressionLevel Optimal
$files = @($zip)
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLower()
Write-Host "Zip:   $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
$checksums = "Zip SHA-256: $sha"
if ($WithSetup) {
  # The installer: the bare Wavy-Flo Setup with the same files packed onto it.
  $bareSetup = Join-Path $app "build-msvc1444\Wavy-Flo Setup.exe"
  if (-not (Test-Path $bareSetup)) { throw "No ${bareSetup} - build first (build-wavy-flo.cmd)." }
  $setup = Join-Path $out "Wavy-Flo-$version-Setup.exe"
  Remove-Item -LiteralPath $setup -Force -ErrorAction SilentlyContinue
  $packing = Start-Process -FilePath $bareSetup -ArgumentList @("--pack", "`"$inside`"", "`"$setup`"", $version) -Wait -PassThru -NoNewWindow
  if ($packing.ExitCode -ne 0 -or -not (Test-Path $setup)) { throw "Packing the installer failed ($($packing.ExitCode))." }
  # The Setup itself, signed whole: its signature covers the files it carries.
  if ($signed) { Sign-WavyFlo @($setup) | Out-Null }
  $files += $setup
  $checksums += "`nSetup SHA-256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLower())"
  Write-Host "Setup: $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
}
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue

$notesFile = Join-Path $app "RELEASE-NOTES.md"
$notes = if (Test-Path $notesFile) { Get-Content -Raw -Encoding UTF8 $notesFile } else { "Wavy-Flo $version" }
# The zip's first: an update reads the first checksum when GitHub gives none.
$notes = $notes.TrimEnd() + "`n`n---`n$checksums`n"
if ($DryRun) {
  Write-Host "`nDry run: nothing uploaded. The notes would be:`n"
  Write-Host $notes
  if (-not $NoSource) { & (Join-Path $PSScriptRoot "publish-source.ps1") -Version $version -Repo $Repo -DryRun }
  exit 0
}
# The source of what is released, for everyone (GPL), first: the release's tag
# is made at the commit that holds it.
if (-not $NoSource) {
  try { & (Join-Path $PSScriptRoot "publish-source.ps1") -Version $version -Repo $Repo }
  catch { throw "Nothing was released: publishing the source failed. $($_.Exception.Message) (-NoSource releases without it.)" }
}
$notesTemp = Join-Path $env:TEMP "wavy-flo-release-notes.md"
[System.IO.File]::WriteAllText($notesTemp, $notes, (New-Object System.Text.UTF8Encoding $false))
& $gh release create "v$version" @files --repo $Repo --title "Wavy-Flo $version" --notes-file $notesTemp
if ($LASTEXITCODE -ne 0) { throw "The upload failed." }
Write-Host "`nReleased: https://github.com/$Repo/releases/tag/v$version"
Write-Host "Every Wavy-Flo older than $version will offer it the next time it opens."
