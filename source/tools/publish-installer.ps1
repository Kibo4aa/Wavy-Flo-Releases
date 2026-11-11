# Publishes the Wavy-Flo Installer: the small Setup that carries no program
# and always fetches and installs the newest Wavy-Flo. It lives in a release
# of its own, tag "installer", never marked as the latest (Wavy-Flo's updates
# read the latest release), at an address that never changes:
#
#   https://github.com/Kibo4aa/Wavy-Flo-Releases/releases/download/installer/Wavy-Flo-Installer.exe
#
# Publish it once. A new Wavy-Flo needs only publish-release.cmd; run this
# again only when the Installer itself has changed.
param([string]$Repo = "Kibo4aa/Wavy-Flo-Releases")
$ErrorActionPreference = "Stop"
$app = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$installer = Join-Path $app "build-msvc1444\Wavy-Flo-Installer.exe"
if (-not (Test-Path $installer)) { throw "No ${installer} - build first (build-wavy-flo.cmd)." }
# Signed, when there is a certificate (signing.txt; see sign.ps1).
. (Join-Path $PSScriptRoot "sign.ps1")
Sign-WavyFlo @($installer) | Out-Null
$gh = "C:\Program Files\GitHub CLI\gh.exe"
if (-not (Test-Path $gh)) { $gh = "gh" }
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash.ToLower()
$notes = @"
**Download this once.** The Wavy-Flo Installer always installs the newest version of Wavy-Flo:
it fetches it as it installs, so this file never goes out of date.

1. Run ``Wavy-Flo-Installer.exe``.
2. Choose where Wavy-Flo goes (your own folder by default, no administrator needed) and press **Install**.
3. Wavy-Flo is in the Start menu, and Windows search finds it.

Windows may say "Windows protected your PC" the first time: choose **More info > Run anyway**.

---
Installer SHA-256: $sha
"@
$notesFile = Join-Path $env:TEMP "wavy-flo-installer-notes.md"
[System.IO.File]::WriteAllText($notesFile, $notes, (New-Object System.Text.UTF8Encoding $false))
$ErrorActionPreference = "Continue"
& $gh release view installer --repo $Repo 2>&1 | Out-Null
$exists = $LASTEXITCODE -eq 0
$ErrorActionPreference = "Stop"
if ($exists) {
  & $gh release upload installer $installer --clobber --repo $Repo
  & $gh release edit installer --repo $Repo --notes-file $notesFile --latest=false
} else {
  & $gh release create installer $installer --repo $Repo --title "Wavy-Flo Installer" --notes-file $notesFile --latest=false
}
if ($LASTEXITCODE -ne 0) { throw "Publishing the Installer failed." }
Write-Host "`nThe Installer: https://github.com/$Repo/releases/download/installer/Wavy-Flo-Installer.exe"
