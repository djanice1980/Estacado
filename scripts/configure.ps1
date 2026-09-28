. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
Initialize-DeveloperToolchain
$source = Join-Path $root 'external/XenonRecomp'
$build = Join-Path $root 'build/XenonRecomp-x64'
if (-not (Test-Path -LiteralPath $source)) { throw "Missing upstream source: $source" }
New-Item -ItemType Directory -Force -Path $build | Out-Null
$cmakeArguments = @('-S', $source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl')
Write-CommandLine ('cmake ' + ($cmakeArguments -join ' '))
& cmake @cmakeArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
