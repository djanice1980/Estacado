. "$PSScriptRoot/_common.ps1"
& "$PSScriptRoot/configure.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$root = Get-ProjectRoot
$buildArguments = @('--build', (Join-Path $root 'build/XenonRecomp-x64'), '--config', 'Release')
Write-CommandLine ('cmake ' + ($buildArguments -join ' '))
& cmake @buildArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
