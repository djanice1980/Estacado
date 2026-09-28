[CmdletBinding()]
param()

. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
New-Item -ItemType Directory -Force (Join-Path $root 'logs') | Out-Null
# The recompiler writes into generated/ppc and does not create it.
New-Item -ItemType Directory -Force (Join-Path $root 'generated/ppc') | Out-Null
$tool = Join-Path $root 'build/XenonRecomp-x64/XenonRecomp/XenonRecomp.exe'
$config = Join-Path $root 'config/darkness_recomp_switch_correction.toml'
$context = Join-Path $root 'external/XenonRecomp/XenonUtils/ppc_context.h'
$xex = Join-Path $root 'Darkness, The (USA, Europe) (En,Fr,De,Es,It)/default.xex'
$log = Join-Path $root 'logs/switch_correction_recomp.log'

foreach ($path in @($tool, $config, $context, $xex)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}
Write-CommandLine ("& '$tool' '$config' '$context'")
$started = Get-Date
"Started: $($started.ToString('o'))" | Tee-Object -FilePath $log
& $tool $config $context 2>&1 | Tee-Object -FilePath $log -Append
$exit = $LASTEXITCODE
$ended = Get-Date
"Ended: $($ended.ToString('o'))" | Tee-Object -FilePath $log -Append
"Exit code: $exit" | Tee-Object -FilePath $log -Append
if ($exit -ne 0) { exit $exit }
