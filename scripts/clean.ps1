[CmdletBinding(SupportsShouldProcess)]
param([switch]$Force)
. "$PSScriptRoot/_common.ps1"
if (-not $Force) { throw 'Refusing to clean without -Force. This removes only ignored output.' }
$root = Get-ProjectRoot
foreach ($relative in @('build', 'generated/ppc', 'logs')) {
    $target = Join-Path $root $relative
    if ($PSCmdlet.ShouldProcess($target, 'Remove ignored output')) {
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $target | Out-Null
    }
}
