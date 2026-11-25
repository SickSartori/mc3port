# Writes the release notes of one version: its section of CHANGELOG.md, then how to download and check it.
#   tools/release_notes.ps1 -Version 0.2.0 -Out notes.md
param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = "Stop"

$lines = Get-Content (Join-Path $PSScriptRoot "..\CHANGELOG.md")
$start = [Array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match "^## $([regex]::Escape($Version))( |$)" })
if ($start -lt 0) { throw "CHANGELOG.md has no section for $Version" }
$end = [Array]::FindIndex($lines, $start + 1, [Predicate[string]] { param($l) $l -match "^## " })
if ($end -lt 0) { $end = $lines.Count }
# GitHub keeps line breaks in release notes: wrapped list items (continuation lines indented) become one line.
$section = ($lines[($start + 1)..($end - 1)] -join "`n") -replace "`n  +", " "

# "Which file?" last: on the release page the files come right under it. Not first: launchers read the
# notes' first list items as what is new.
$download = @"
### Which file?

- **Windows**: ``mc3port-$Version-windows-setup.exe``. Run it: it installs for your user, and the launcher tells you about updates. It is code signed; the certificate is new, so the browser or Windows may still warn ("Keep", then "More info" and "Run anyway").
- **Windows, without installing**: ``mc3port-$Version-windows-portable.zip``. Extract it anywhere and start ``mc3.exe``.
- **Linux**: ``mc3port-$Version-linux.tar.gz`` (Wine; see the README).
- Or install it from [RecompLauncher](https://recomplauncher.com/game/mc3port), which updates it for you.
- You need your own disc image of the European or USA Xbox release: see the README. ``SHA256SUMS`` is for checking a download; the two ``Source code`` files are only the code.
- [Code signing policy](https://github.com/SickSartori/mc3port#code-signing-policy) and [Privacy](https://github.com/SickSartori/mc3port#privacy).
"@
$notes = $section.Trim() + "`n`n" + $download.Trim()
Set-Content -Path $Out -Value $notes -Encoding utf8NoBOM
