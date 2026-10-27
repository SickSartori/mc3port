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

$notes = $section.Trim() + @"


### Download

- **Installer**: ``mc3port-$Version-windows-x86-setup.exe`` (installs for your user; the launcher updates it).
- **Portable**: ``mc3port-$Version-windows-x86.zip`` (extract anywhere and start ``mc3.exe``).
- Check a download against ``SHA256SUMS``. You need your own disc image of the European Xbox release: see the README.
"@
Set-Content -Path $Out -Value $notes -Encoding utf8NoBOM
