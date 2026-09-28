param([string]$XexPath = (Join-Path $PSScriptRoot '../Darkness, The (USA, Europe) (En,Fr,De,Es,It)/default.xex'))
. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
New-Item -ItemType Directory -Force (Join-Path $root 'logs') | Out-Null
$tool = Join-Path $root 'build/XenonRecomp-x64/XenonAnalyse/XenonAnalyse.exe'
$out = Join-Path $root 'config/darkness_switch_tables.toml'
if (-not (Test-Path -LiteralPath $XexPath -PathType Leaf)) { throw "XEX not found: $XexPath" }
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "XenonAnalyse not built: $tool. Run scripts/build.ps1 first." }
Write-CommandLine ("& '$tool' '$XexPath' '$out'")
$timer = [Diagnostics.Stopwatch]::StartNew()
& $tool $XexPath $out 2>&1 | Tee-Object -FilePath (Join-Path $root 'logs/XenonAnalyse.stdout-stderr.log')
$exit = $LASTEXITCODE
$timer.Stop()
"Elapsed: $($timer.Elapsed)" | Tee-Object -FilePath (Join-Path $root 'logs/XenonAnalyse.timing.log')
if ($exit -ne 0) { exit $exit }
