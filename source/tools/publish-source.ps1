# Publishes Wavy-Flo's source in the repository its downloads come from,
# Kibo4aa/Wavy-Flo-Releases: the folder source/ beside the download page, and
# the licence at the top so GitHub shows it.
#
# Development happens in a private repository whose history stays there. Each
# release's tree goes into source/ as one commit on top of the public main: the
# files exactly as they were committed, never what is still uncommitted in this
# folder. Everything else there -- the download page, its pictures -- stays as
# it is. Nothing here is checked out, reset or moved: the commit is made from
# trees alone, in an index of its own.
#
#   publish-source.ps1                    the tree of the tag v<version>, or HEAD
#   publish-source.ps1 -Ref <commit>      another commit's tree
#   publish-source.ps1 -DryRun            show what would be published, push nothing
#
# A pull request merged there that changed source/ stays in its history, with
# its author. Bring it into the private repository first (from this folder:
# git diff <last snapshot> FETCH_HEAD -- source | git apply -p2), then publish
# with -TakeOverPublicChanges: the script stops otherwise, so that publishing
# never undoes it.
param(
  [string]$Version = "",
  [string]$Ref = "",
  [string]$Repo = "Kibo4aa/Wavy-Flo-Releases",
  [string]$Folder = "source",
  [switch]$DryRun,
  [switch]$TakeOverPublicChanges
)
$ErrorActionPreference = "Stop"
$source = Split-Path -Parent $PSScriptRoot   # ...\Wavy-Flo\source
$marker = "Wavy-Flo-Source-Snapshot:"
# git's output lines; what it writes to stderr is only shown when it fails.
# (Windows PowerShell stops on any stderr line when errors are set to stop.)
# Named apart from git itself: PowerShell finds a function before a program,
# whatever its case, and a function called Git called itself.
function Invoke-Git {
  $previous = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  $out = & git.exe -C $source @args 2>&1
  $code = $LASTEXITCODE
  $ErrorActionPreference = $previous
  if ($code -ne 0) { throw "git $($args -join ' ') failed: $($out -join ' ')" }
  return @($out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] } | ForEach-Object { "$_" })
}
# The object a name stands for, or nothing when there is none.
function Find-Object([string]$name) {
  try { return @(Invoke-Git rev-parse -q --verify $name)[0].Trim() } catch { return $null }
}
if ($Version -eq "") {
  $match = Select-String -Path (Join-Path $source "src\updater.h") -Pattern 'kWavyFloVersion\s*=\s*"([^"]+)"'
  if (-not $match) { throw "No kWavyFloVersion in src\updater.h" }
  $Version = $match.Matches[0].Groups[1].Value
}
# What is published: the release's own tag when there is one.
if ($Ref -eq "") {
  $Ref = if (Find-Object "refs/tags/v$Version^{commit}") { "refs/tags/v$Version" } else { "HEAD" }
  if ($Ref -eq "HEAD") { Write-Warning "No tag v${Version}: publishing HEAD." }
}
$commit = Find-Object "$Ref^{commit}"
if (-not $commit) { throw "No commit $Ref" }
$tree = Find-Object "$Ref^{tree}"
$license = Find-Object "${Ref}:LICENSE"
if (-not $license) { throw "$Ref has no LICENSE" }
$url = "https://github.com/$Repo.git"
Write-Host "Wavy-Flo $Version source: $Ref ($($commit.Substring(0, 8))), tree $($tree.Substring(0, 8))"

# Where the public repository stands, and what the last snapshot put in it.
$publicMain = @(Invoke-Git ls-remote $url refs/heads/main) | ForEach-Object { ($_ -split "\s+")[0] } | Select-Object -First 1
if (-not $publicMain) { throw "$Repo has no main branch." }
Invoke-Git fetch --quiet $url main | Out-Null
$current = Find-Object "FETCH_HEAD:$Folder"
if ($current -eq $tree) { Write-Host "$Repo already holds this source."; exit 0 }
$lastSnapshot = @(Invoke-Git log -1 "--grep=$marker" --format=%H FETCH_HEAD) | Select-Object -First 1
$published = if ($lastSnapshot) { Find-Object "${lastSnapshot}:$Folder" } else { $null }
if ($current -ne $published -and -not $TakeOverPublicChanges) {
  throw ("$Folder/ in $Repo was changed after the last snapshot: someone's change was merged there. " +
         "Bring it into the private repository first (git diff $lastSnapshot FETCH_HEAD -- $Folder | git apply -p2), " +
         "then run again with -TakeOverPublicChanges.")
}
if ($DryRun) {
  Write-Host "Dry run: would put tree $($tree.Substring(0, 8)) in $Repo/$Folder and LICENSE beside the page, after $($publicMain.Substring(0, 8))."
  exit 0
}

# The public tree with source/ and LICENSE replaced, built in an index of its
# own: this folder's index and files are never touched.
$index = Join-Path $env:TEMP ("wavy-flo-source-index-" + [Guid]::NewGuid().ToString("N"))
$env:GIT_INDEX_FILE = $index
try {
  Invoke-Git read-tree FETCH_HEAD | Out-Null
  Invoke-Git rm --cached -r -q --ignore-unmatch -- $Folder LICENSE | Out-Null
  Invoke-Git read-tree "--prefix=$Folder/" $tree | Out-Null
  Invoke-Git update-index --add --cacheinfo "100644,$license,LICENSE" | Out-Null
  $root = @(Invoke-Git write-tree)[0].Trim()
} finally {
  Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $index -Force -ErrorAction SilentlyContinue
}
$message = "Source of Wavy-Flo $Version`n`nThe source of Wavy-Flo $Version in $Folder/, under the GNU General Public`n" +
           "License, version 3 or later.`n`n$marker $Version`n"
$messageFile = Join-Path $env:TEMP "wavy-flo-source-message.txt"
[System.IO.File]::WriteAllText($messageFile, $message, (New-Object System.Text.UTF8Encoding $false))
$snapshot = @(Invoke-Git commit-tree $root -p $publicMain -F $messageFile)[0].Trim()
Remove-Item $messageFile -Force
Invoke-Git push --quiet $url "${snapshot}:refs/heads/main" | Out-Null
Write-Host "Published: https://github.com/$Repo/tree/main/$Folder ($($snapshot.Substring(0, 8)))"
