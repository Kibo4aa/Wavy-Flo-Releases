# Signs Wavy-Flo's own programs with its code signing certificate, so Windows
# shows who made them ("Verified publisher") and antivirus programs trust them.
#
# The certificate is chosen in signing.txt, beside the Wavy-Flo folder's
# build-wavy-flo.cmd -- never in the source, never in git. One line of what
# signtool needs to find it, for example
#
#   /sha1 0123456789ABCDEF0123456789ABCDEF01234567
#
# for a certificate in Windows' store (a USB token, or Certum's SimplySign,
# puts it there: the thumbprint is under its Details), and optionally
#
#   timestamp=http://time.certum.pl
#
# Lines starting with # are ignored. Without signing.txt nothing is signed and
# the release goes out unsigned, with a warning.
#
#   . sign.ps1; Sign-WavyFlo @("a.exe", "b.dll")   # $true when signed
function Find-SignTool {
  $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
  $tool = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*\x64\*" } | Sort-Object FullName -Descending | Select-Object -First 1
  if ($tool) { return $tool.FullName }
  return $null
}
function Sign-WavyFlo([string[]]$Files) {
  $app = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $config = if ($env:WAVY_FLO_SIGNING) { $env:WAVY_FLO_SIGNING } else { Join-Path $app "signing.txt" }
  if (-not (Test-Path $config)) {
    Write-Warning "Not signed: no $config (see source\tools\sign.ps1)."
    return $false
  }
  $selection = @(); $timestamp = "http://timestamp.digicert.com"
  foreach ($line in Get-Content $config) {
    $line = $line.Trim()
    if ($line -eq "" -or $line.StartsWith("#")) { continue }
    if ($line -like "timestamp=*") { $timestamp = $line.Substring(10).Trim(); continue }
    # Arguments as written, quoted parts kept together.
    $selection += [regex]::Matches($line, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') }
  }
  $signtool = Find-SignTool
  if (-not $signtool) { throw "signtool.exe not found: install the Windows SDK." }
  $files = @($Files | Where-Object { Test-Path $_ })
  if (-not $files.Count) { return $true }
  $arguments = @("sign") + $selection + @("/fd", "sha256", "/d", "Wavy-Flo", "/du", "https://github.com/Kibo4aa/Wavy-Flo-Releases")
  if ($timestamp -ne "none") { $arguments += @("/tr", $timestamp, "/td", "sha256") }
  & $signtool @arguments @files | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "Signing failed ($LASTEXITCODE)." }
  Write-Host "Signed $($files.Count) file(s)."
  return $true
}
