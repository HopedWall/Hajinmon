# Builds the release: a single build\release\Hajinmon-<version>-x86_64.exe that contains the
# launcher and the game runtime (HajinmonGame.exe and its DLLs). It contains no game
# data: players point it at their own copy of the game.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 [-SkipGameBuild]
# Requires everything needed to build the game (see README.md) and the .NET 9 SDK.

param([switch]$SkipGameBuild)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$build = Join-Path $root "doa4\out\build\win-amd64-relwithdebinfo"
$sdk = Join-Path $root "external\rexglue-sdk"
$out = Join-Path $root "build\release"
$stage = Join-Path $out "runtime"

if (-not $SkipGameBuild) {
    Write-Host "[1/4] Building the game..."
    & cmd /c "`"$root\tools\build_doa4.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "Game build failed" }
} else {
    Write-Host "[1/4] Using the existing game build."
}

Write-Host "[2/4] Collecting the runtime..."
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# The recompiled game and the ReXGlue runtime.
foreach ($file in "HajinmonGame.exe", "rexruntimerd.dll", "rexgpu-xenosrd.dll", "TracyClientrd.dll") {
    Copy-Item (Join-Path $build $file) $stage
}
# The shader compiler of the native (Plume) renderer, in builds that have it.
foreach ($file in "dxcompiler.dll", "dxil.dll") {
    $path = Join-Path $build $file
    if (Test-Path $path) { Copy-Item $path $stage }
}
$plumeLicense = Join-Path $root "external\plume\LICENSE"
if (Test-Path $plumeLicense) { Copy-Item $plumeLicense (Join-Path $stage "LICENSE-plume.txt") }
$fsrLicense = Join-Path $root "external\FidelityFX-FSR\license.txt"
if (Test-Path $fsrLicense) { Copy-Item $fsrLicense (Join-Path $stage "LICENSE-fidelityfx-fsr.txt") }
if (Test-Path (Join-Path $stage "dxcompiler.dll")) {
    Copy-Item (Join-Path $root "renderer\LICENSE-dxc.txt") (Join-Path $stage "LICENSE-dxc.txt")
}

# Visual C++ runtime, app-local, so players do not need to install it.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$redist = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Directory |
    Where-Object { $_.Name -match '^\d' } | Sort-Object Name -Descending | Select-Object -First 1
$crt = Get-ChildItem (Join-Path $redist.FullName "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
foreach ($dll in "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $crt.FullName $dll) $stage
}

# Licenses: Hajinmon's, and those of the SDK and its third-party libraries.
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $sdk "LICENSE") (Join-Path $stage "LICENSE-rexglue-sdk.txt")
& py -I (Join-Path $root "tools\third_party_notices.py") $sdk (Join-Path $stage "THIRD_PARTY_NOTICES.txt")
if ($LASTEXITCODE -ne 0) { throw "Could not write the third-party notices" }

# Guard: the release must never contain game data.
$forbidden = Get-ChildItem $stage -Recurse -File |
    Where-Object { $_.Extension -in ".xex", ".iso", ".afs", ".bin", ".wmv", ".xma", ".xwb" }
if ($forbidden) { throw "Game data found in the release: $($forbidden.Name -join ', ')" }

$zip = Join-Path $out "game_runtime.zip"
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

Write-Host "[3/4] Building the launcher..."
$publish = Join-Path $out "publish"
& dotnet publish (Join-Path $root "launcher\Hajinmon.Launcher.csproj") -c Release -o $publish `
    -p:SelfContained=true -p:EnableCompressionInSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=none `
    "-p:GameRuntimeZip=$zip"
if ($LASTEXITCODE -ne 0) { throw "Launcher build failed" }

Write-Host "[4/4] Finishing..."
# Named like the Linux AppImage: Hajinmon-<version>-x86_64.exe.
$version = ([xml](Get-Content (Join-Path $root "launcher\Hajinmon.Launcher.csproj"))).Project.PropertyGroup.Version |
    Where-Object { $_ } | Select-Object -First 1
$name = "Hajinmon-$version-x86_64.exe"
Copy-Item (Join-Path $publish "Hajinmon.exe") (Join-Path $out $name)
Remove-Item $publish, $stage -Recurse -Force
$size = [math]::Round((Get-Item (Join-Path $out $name)).Length / 1MB, 1)
Write-Host "Release ready: $out\$name ($size MB)"
