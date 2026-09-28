[CmdletBinding()]
param(
    # Your game: the disc image (.iso) or a folder with the extracted files
    # (the folder that contains default.xex). Not needed when the extracted
    # game is already in "Darkness, The (USA, Europe) (En,Fr,De,Es,It)" next to
    # this repository.
    [string]$Game = '',
    # Download the optional upscaler SDKs (NVIDIA DLSS, AMD FidelityFX, Intel
    # XeSS) after you accepted their licences (scripts/fetch-sdks.ps1).
    [switch]$FetchSdks,
    [switch]$AcceptSdkLicenses,
    # Which SDKs to download (default all three).
    [string[]]$Sdk = @('dlss', 'fsr', 'xess'),
    # Player package folder to create at the end (empty = no package).
    [string]$Package = '',
    [switch]$Zip,
    # Skip the regression tests (they take a few minutes).
    [switch]$SkipTests,
    # Release builds: the release's language pack download (HTTPS URL
    # and SHA-256), offered by the launcher as "Download and install".
    [string]$LanguagePackUrl = '',
    [string]$LanguagePackSha256 = ''
)
# Builds the whole port on your PC from your own copy of the game:
#   1. checks the game version (default.xex SHA-256) and, for a disc image,
#      extracts it;
#   2. builds XenonRecomp and recompiles your default.xex into generated/ppc;
#   3. (optional) downloads the upscaler SDKs;
#   4. builds ReXGlue, the runtime (TheDarkness.exe, TheDarknessSettings.exe)
#      and the tools and tests, then runs the tests;
#   5. (optional) packages the result.
# Everything derived from the game stays on your PC: the extracted files,
# generated/ppc and the executable built from it are for your own use.
. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
# "-Sdk fsr,xess" arrives as one string through powershell -File.
$Sdk = @($Sdk | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } |
    Where-Object { $_ })
foreach ($name in $Sdk) {
    if ($name -notin 'dlss', 'fsr', 'xess') { throw "Unknown SDK '$name' (use dlss, fsr, xess)." }
}
Initialize-DeveloperToolchain
$supportedXex = 'AACE35A8F9BCDC7F28AEAB9FF8CF3BDF200353F5C83705F6284487347ACB3C5F'
$gameFolderName = 'Darkness, The (USA, Europe) (En,Fr,De,Es,It)'
$gameFolder = Join-Path $root $gameFolderName

function Step([string]$Text) { Write-Host ''; Write-Host "== $Text" }
function Check-Exit([string]$What) { if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" } }

Step 'Sources'
& git -C $root submodule update --init --recursive
Check-Exit 'git submodule update'

Step 'Game files'
if ($Game) {
    $gamePath = [IO.Path]::GetFullPath($Game)
    if (Test-Path -LiteralPath $gamePath -PathType Leaf) {
        if (Test-Path -LiteralPath $gameFolder) { throw "$gameFolder already exists; remove it or pass the folder instead." }
        # The launcher's game setup as a small command-line tool.
        $tools = Join-Path $root 'build/game_setup'
        & cmake -S (Join-Path $root 'tools/game_setup') -B $tools -G Ninja -DCMAKE_BUILD_TYPE=Release `
            -DCMAKE_CXX_COMPILER=clang-cl
        Check-Exit 'game-setup tool configure'
        & cmake --build $tools --config Release
        Check-Exit 'game-setup tool build'
        & (Join-Path $tools 'darkness_game_setup.exe') extract $gamePath $gameFolder
        Check-Exit 'disc image extraction'
    } elseif (Test-Path -LiteralPath (Join-Path $gamePath 'default.xex') -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $gameFolder)) {
            # A junction keeps the recompiler configurations' relative path.
            & cmd /c mklink /J "`"$gameFolder`"" "`"$gamePath`"" | Out-Host
            Check-Exit 'junction to the game folder'
        }
    } else {
        throw "$gamePath is neither a disc image nor a folder with default.xex"
    }
}
$xex = Join-Path $gameFolder 'default.xex'
if (-not (Test-Path -LiteralPath $xex -PathType Leaf)) {
    throw "No game files: pass -Game <disc image or extracted folder>."
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $xex).Hash
if ($hash -ne $supportedXex) {
    throw "default.xex SHA-256 $hash is not the supported version (The Darkness, Xbox 360, USA/Europe disc)."
}
Write-Host "default.xex: supported version"

Step 'Recompiler'
& (Join-Path $PSScriptRoot 'build.ps1')
Check-Exit 'XenonRecomp build'
& (Join-Path $PSScriptRoot 'analyse.ps1')
Check-Exit 'XEX analysis'
& (Join-Path $PSScriptRoot 'recomp-switch-correction.ps1')
Check-Exit 'recompilation'

if ($FetchSdks) {
    Step 'SDKs'
    & (Join-Path $PSScriptRoot 'fetch-sdks.ps1') -AcceptLicenses:$AcceptSdkLicenses -Sdk $Sdk
    Check-Exit 'SDK download'
}

Step 'ReXGlue'
& (Join-Path $PSScriptRoot 'build-rexglue.ps1') -Configure
Check-Exit 'ReXGlue build'

Step 'Runtime'
$runtime = Join-Path $root 'build/runtime'
& cmake -S (Join-Path $root 'runtime') -B $runtime -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    "-DDARKNESS_LANGUAGE_PACK_URL=$LanguagePackUrl" "-DDARKNESS_LANGUAGE_PACK_SHA256=$LanguagePackSha256"
Check-Exit 'runtime configure'
& cmake --build $runtime --config Release
Check-Exit 'runtime build'

if (-not $SkipTests) {
    Step 'Tests'
    $tests = Join-Path $root 'build/register_helpers'
    & cmake -S (Join-Path $root 'tests/register_helpers') -B $tests -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
    Check-Exit 'tests configure'
    & cmake --build $tests --config Release
    Check-Exit 'tests build'
    & ctest --test-dir $tests -C Release --output-on-failure
    Check-Exit 'tests'
}

if ($Package) {
    Step 'Package'
    & (Join-Path $PSScriptRoot 'package.ps1') -Output $Package -Zip:$Zip
    Check-Exit 'packaging'
}
Step 'Done'
Write-Host "Run build/runtime/TheDarknessSettings.exe (or the package's) to play."
