# Builds the Release configuration and packages it for a GitHub release:
#   dist/mc3port-<version>-windows-x86.zip  (mc3.exe, mc3runtime.dll, readme and licences)
#   dist/SHA256SUMS
# The version comes from project(... VERSION ...) in CMakeLists.txt. No game file can end up in the package.
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
}

$name = "mc3port-$version-windows-x86"
$stage = Join-Path "dist" $name
if (Test-Path dist) { Remove-Item dist -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
Copy-Item build/pc-x86/Release/mc3.exe, build/pc-x86/Release/mc3runtime.dll, README.md, LICENSE, THIRD_PARTY.md $stage
Copy-Item third_party/imgui/LICENSE.txt "$stage\licenses\imgui-LICENSE.txt"
Copy-Item third_party/fonts/OFL.txt "$stage\licenses\barlow-OFL.txt"

$forbidden = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in ".iso", ".xbe", ".xiso", ".log", ".ini" }
if ($forbidden) { throw "not for the package: $($forbidden.Name -join ', ')" }

$zip = "dist/$name.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path dist/SHA256SUMS -Value "$hash  $name.zip" -Encoding ascii -NoNewline
"$zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
"sha256 $hash"
