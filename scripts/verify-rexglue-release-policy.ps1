[CmdletBinding()]
param(
    [string]$BuildDirectory =
        'external/ReXGlue/out/build/win-amd64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$candidateBuild = Join-Path $root $BuildDirectory
$resolvedBuild = (Resolve-Path -LiteralPath $candidateBuild).Path
$cachePath = Join-Path $resolvedBuild 'CMakeCache.txt'
$releaseNinja = Join-Path $resolvedBuild 'build-Release.ninja'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $releaseNinja -PathType Leaf)) {
    throw "ReXGlue Release build is incomplete: $resolvedBuild"
}

$makeProgramLine = Get-Content -LiteralPath $cachePath |
    Where-Object { $_ -like 'CMAKE_MAKE_PROGRAM:FILEPATH=*' } |
    Select-Object -First 1
if (-not $makeProgramLine) {
    throw "ReXGlue build has no CMAKE_MAKE_PROGRAM: $cachePath"
}
$ninja = $makeProgramLine.Split('=', 2)[1]

$rows = foreach ($target in @('rexruntime.dll:Release',
                               'rexgpu-xenos.dll:Release')) {
    $commands = & $ninja -C $resolvedBuild -f $releaseNinja `
        -t commands $target 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect ReXGlue target $target`: $commands"
    }
    $outputName = $target.Split(':', 2)[0]
    $linkCommand = $commands | Where-Object {
        $_ -match ('(?i)(?:/out:|-o\s+)\S*' + [regex]::Escape($outputName))
    } | Select-Object -Last 1
    if (-not $linkCommand) {
        throw "ReXGlue link command not found for $target"
    }
    if ($linkCommand -notmatch '(?i)(^|\s)(-Xlinker\s+)?/Brepro(\s|$)') {
        throw "ReXGlue link command is missing /Brepro for $target"
    }
    [pscustomobject]@{
        Target = $target
        DeterministicPeTimestamp = '/Brepro'
        Status = 'PASS'
    }
}

$rows | Format-Table -AutoSize
