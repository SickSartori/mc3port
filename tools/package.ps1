# Builds the Release configuration and packages it for a GitHub release:
#   dist/mc3port-<version>-windows-setup.exe     (installer, with Inno Setup 6.6+; skipped if it is missing. Older
#                                                 launchers find their update by the "-setup.exe" ending)
#   dist/mc3port-<version>-windows-portable.zip  (a folder "mc3port" with mc3.exe, mc3runtime.dll, README.txt, and the
#                                                 other documents in docs; the same folder name in every version, so a
#                                                 new zip extracted in the same place updates the old one)
#   dist/mc3port-<version>-linux.tar.gz          (the portable folder plus install.sh for Wine, and the menu icon)
#   dist/SHA256SUMS
# The version comes from project(... VERSION ...) in CMakeLists.txt. No game file can end up in the package.
# PowerShell 7 (pwsh), as in the workflow: Windows PowerShell 5's Compress-Archive writes backslashes in the zip's
# paths, which Linux's unzip and other tools read wrongly.
#
# -Sign signs mc3.exe, mc3runtime.dll, the installer and its uninstaller with the code signing certificate of this PC
# (Certum, through SimplySign Desktop: its session must be open). GitHub Actions cannot sign, so a release is signed
# here from what the workflow built: download the draft release's portable zip, then
#   pwsh tools/package.ps1 -From <that zip> -Sign
#   gh release upload v<version> (Get-ChildItem dist -File).FullName --clobber
# -From <portable zip> packages that zip's folder instead of building one.
#Requires -Version 7
param([switch]$SkipBuild, [string]$From, [switch]$Sign)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($From) {
    $From = (Resolve-Path $From).Path  # before the folder changes; dist is emptied below
    if ($From.StartsWith((Join-Path $root "dist"), [StringComparison]::OrdinalIgnoreCase)) { throw "-From: keep the zip outside dist, which is emptied" }
}
Set-Location $root

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(mc3port VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if (-not $version) { throw "no version in CMakeLists.txt" }

if ($Sign) {
    # The one code signing certificate in the user's store; the newest signtool of the Windows SDK.
    $certificate = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert)
    if ($certificate.Count -ne 1) { throw "-Sign needs exactly one code signing certificate, found $($certificate.Count): is SimplySign Desktop's session open?" }
    $signtool = (Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName -Descending | Select-Object -First 1).FullName
    if (-not $signtool) { throw "signtool.exe not found: it comes with the Windows SDK" }
    # SHA-256, and Certum's time stamp: the signature stays valid after the certificate expires.
    $signing = @("sign", "/sha1", $certificate[0].Thumbprint, "/fd", "SHA256", "/tr", "http://time.certum.pl", "/td", "SHA256")
}
function Sign-File([string[]]$Files) {
    & $signtool @signing @Files; if ($LASTEXITCODE) { throw "signing failed" }
    & $signtool verify /pa /q @Files; if ($LASTEXITCODE) { throw "a signature does not verify" }
}

if (-not $SkipBuild -and -not $From) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
        $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    }
    if (-not (Test-Path build/pc-x86/CMakeCache.txt)) { & $cmake -S . -B build/pc-x86 -A Win32; if ($LASTEXITCODE) { throw "configure failed" } }
    & $cmake --build build/pc-x86 --config Release; if ($LASTEXITCODE) { throw "build failed" }
    & (Join-Path (Split-Path $cmake) ctest.exe) --test-dir build/pc-x86 -C Release --output-on-failure; if ($LASTEXITCODE) { throw "tests failed" }
}

$name = "mc3port-$version-windows"
$stage = Join-Path "dist" "mc3port"
if (Test-Path dist) { Remove-Item dist -Recurse -Force }
if ($From) {
    # What the workflow built and packaged: the zip holds the folder "mc3port".
    Expand-Archive -Path $From -DestinationPath dist
    if (-not (Test-Path "$stage\mc3.exe")) { throw "$From does not hold mc3port\mc3.exe" }
} else {
    New-Item -ItemType Directory -Force "$stage\docs\licenses" | Out-Null
    Copy-Item build/pc-x86/Release/mc3.exe, build/pc-x86/Release/mc3runtime.dll, recomplauncher.json $stage
    # Português (Brasil): differences from the disc's files, and their credits and licence (translations/README.md)
    New-Item -ItemType Directory -Force "$stage\translations" | Out-Null
    Copy-Item translations/pt-br.mc3patch, translations/README.md "$stage\translations"
    # The HD HUD's pictures (hud_redrawn/README.md)
    New-Item -ItemType Directory -Force "$stage\hud_redrawn" | Out-Null
    Copy-Item hud_redrawn/*.png, hud_redrawn/README.md "$stage\hud_redrawn"
    Copy-Item README.md, CHANGELOG.md, LICENSE, THIRD_PARTY.md "$stage\docs"
    # The PS2 loading music's guide, where the README's link (loading_music/README.md) finds it
    New-Item -ItemType Directory -Force "$stage\docs\loading_music" | Out-Null
    Copy-Item loading_music/README.md "$stage\docs\loading_music"
    # README.txt: the short guide (what you need, first start, where the files are), with the version filled in and
    # Windows line endings, so that Notepad shows it properly.
    $readme = ((Get-Content tools/package_readme.txt -Raw).Replace("<version>", $version) -replace "`r?`n", "`r`n")
    [IO.File]::WriteAllText((Join-Path (Resolve-Path $stage) "README.txt"), $readme, (New-Object Text.UTF8Encoding $false))
    Copy-Item third_party/imgui/LICENSE.txt "$stage\docs\licenses\imgui-LICENSE.txt"
    Copy-Item third_party/fonts/OFL.txt "$stage\docs\licenses\barlow-OFL.txt"
    Copy-Item third_party/sdl3/LICENSE.txt "$stage\docs\licenses\sdl3-LICENSE.txt"
}

$forbidden = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in ".iso", ".xbe", ".xiso", ".log", ".ini" }
if ($forbidden) { throw "not for the package: $($forbidden.Name -join ', ')" }

if ($Sign) { Sign-File "$stage\mc3.exe", "$stage\mc3runtime.dll" }

Compress-Archive -Path $stage -DestinationPath "dist/$name-portable.zip"

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
    # Signed: Inno Setup signs the uninstaller it puts inside, then the installer ($q is a quote, $f the file, to it).
    $signed = if ($Sign) { "/DSign", ("/Ssign=`$q$signtool`$q $($signing -join ' ') `$f") } else { @() }
    & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$(Resolve-Path $stage)" "/DOutputDir=$(Resolve-Path dist)" @signed installer/mc3port.iss
    if ($LASTEXITCODE) { throw "installer build failed" }
    if ($Sign) { & $signtool verify /pa /q "dist/$name-setup.exe"; if ($LASTEXITCODE) { throw "the installer's signature does not verify" } }
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
