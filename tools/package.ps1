# Builds the Release configuration and packages it for a GitHub release:
#   dist/mc3port-<version>-windows-x86.zip        (portable: a folder "mc3port" with mc3.exe, mc3runtime.dll, README.txt,
#                                                  and the other documents in docs; the same folder name in every version, so a
#                                                  new zip extracted in the same place updates the old one)
#   dist/mc3port-<version>-windows-x86-setup.exe  (installer, with Inno Setup 6.6+; skipped if it is missing)
#   dist/mc3port-<version>-linux.tar.gz           (the portable folder plus install.sh for Wine, and the menu icon)
#   dist/SHA256SUMS
# The version comes from project(... VERSION ...) in CMakeLists.txt. No game file can end up in the package.
# PowerShell 7 (pwsh), as in the workflow: Windows PowerShell 5's Compress-Archive writes backslashes in the zip's
# paths, which Linux's unzip and other tools read wrongly.
#Requires -Version 7
param([switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(mc3port VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "no version in CMakeLists.txt" }

if (-not $SkipBuild) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
        $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    }
    if (-not (Test-Path build/pc-x86/CMakeCache.txt)) { & $cmake -S . -B build/pc-x86 -A Win32; if ($LASTEXITCODE) { throw "configure failed" } }
    & $cmake --build build/pc-x86 --config Release; if ($LASTEXITCODE) { throw "build failed" }
    & (Join-Path (Split-Path $cmake) ctest.exe) --test-dir build/pc-x86 -C Release --output-on-failure; if ($LASTEXITCODE) { throw "tests failed" }
}

$name = "mc3port-$version-windows-x86"
$stage = Join-Path "dist" "mc3port"
if (Test-Path dist) { Remove-Item dist -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\docs\licenses" | Out-Null
Copy-Item build/pc-x86/Release/mc3.exe, build/pc-x86/Release/mc3runtime.dll, recomplauncher.json $stage
Copy-Item README.md, CHANGELOG.md, LICENSE, THIRD_PARTY.md "$stage\docs"
# README.txt: the short guide (what you need, first start, where the files are), with the version filled in and
# Windows line endings, so that Notepad shows it properly.
$readme = ((Get-Content tools/package_readme.txt -Raw).Replace("<version>", $version) -replace "`r?`n", "`r`n")
[IO.File]::WriteAllText((Join-Path (Resolve-Path $stage) "README.txt"), $readme, (New-Object Text.UTF8Encoding $false))
Copy-Item third_party/imgui/LICENSE.txt "$stage\docs\licenses\imgui-LICENSE.txt"
Copy-Item third_party/fonts/OFL.txt "$stage\docs\licenses\barlow-OFL.txt"
Copy-Item third_party/sdl3/LICENSE.txt "$stage\docs\licenses\sdl3-LICENSE.txt"

$forbidden = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in ".iso", ".xbe", ".xiso", ".log", ".ini" }
if ($forbidden) { throw "not for the package: $($forbidden.Name -join ', ')" }

Compress-Archive -Path $stage -DestinationPath "dist/$name.zip"

# Linux (Wine): the same folder plus install.sh (Wine prefix, "mc3port" command, menu entry) and the icon for the
# menu, as a tar.gz.
$linux = Join-Path "dist" "linux\mc3port"
New-Item -ItemType Directory -Force $linux | Out-Null
Copy-Item "$stage\*" $linux -Recurse
Copy-Item tools/install-linux.sh "$linux\install.sh"
Copy-Item docs/logo.png "$linux\mc3port.png"
tar -czf "dist/mc3port-$version-linux.tar.gz" -C dist/linux mc3port; if ($LASTEXITCODE) { throw "tar failed" }
Remove-Item dist/linux -Recurse -Force

$iscc = @($env:ISCC, "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($iscc) {
    & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$(Resolve-Path $stage)" "/DOutputDir=$(Resolve-Path dist)" installer/mc3port.iss
    if ($LASTEXITCODE) { throw "installer build failed" }
} elseif ($env:GITHUB_ACTIONS) {
    throw "Inno Setup not found: the release needs the installer"
} else {
    Write-Warning "Inno Setup not found: no installer"
}

$lines = foreach ($file in Get-ChildItem dist -File | Where-Object { $_.Extension -in ".zip", ".exe", ".gz" } | Sort-Object Name) {
    $hash = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLower()
    "$($file.Name)  ($([math]::Round($file.Length / 1MB, 1)) MB)  sha256 $hash" | Write-Host
    "$hash  $($file.Name)"
}
Set-Content -Path dist/SHA256SUMS -Value ($lines -join "`n") -Encoding ascii -NoNewline
