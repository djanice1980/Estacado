. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
Assert-CommandAvailable ctest
$candidates = @(
    (Join-Path $root 'build/register_helpers'),
    (Join-Path $root 'build/XenonRecomp-x64')
)
$build = $candidates | Where-Object {
    Test-Path -LiteralPath (Join-Path $_ 'CTestTestfile.cmake') -PathType Leaf
} | Select-Object -First 1
if (-not $build) {
    throw "No configured project test tree. Expected one of: $($candidates -join ', ')"
}
$testArguments = @('--test-dir', $build, '--output-on-failure')
Write-CommandLine ('ctest ' + ($testArguments -join ' '))
& ctest @testArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
