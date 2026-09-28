[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$ninjaPath = Join-Path $resolvedBuild 'build.ninja'
if (-not (Test-Path -LiteralPath $ninjaPath -PathType Leaf)) {
    throw "Runtime build does not contain build.ninja: $resolvedBuild"
}

$lines = Get-Content -LiteralPath $ninjaPath

function Test-ReleaseTargetPolicy {
    param([Parameter(Mandatory)][string]$Target)

    $objectRules = [System.Collections.Generic.List[object]]::new()
    $objectPattern = '^build CMakeFiles\\' + [regex]::Escape($Target) +
        '\.dir\\.*\.obj:'
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index] -notmatch $objectPattern) { continue }
        $ruleFlags = $null
        $ruleDefines = ''
        for ($ruleIndex = $index + 1;
             $ruleIndex -lt [Math]::Min($index + 12, $lines.Count);
             ++$ruleIndex) {
            if ($lines[$ruleIndex] -match '^  DEFINES = (.*)$') {
                $ruleDefines = $Matches[1]
                continue
            }
            if ($lines[$ruleIndex] -match '^  FLAGS = (.*)$') {
                $ruleFlags = $Matches[1]
                break
            }
        }
        if ($null -ne $ruleFlags) {
            $objectRules.Add([pscustomobject]@{
                Defines = $ruleDefines
                Flags = $ruleFlags
            })
        }
    }
    if (-not $objectRules.Count) {
        throw "No $Target object compile rules were found in $ninjaPath"
    }
    $missingCompilePolicy = $objectRules | Where-Object {
        $_.Flags -notmatch '(^|\s)/O2(\s|$)' -or
        $_.Flags -notmatch '(^|\s)/Ob2(\s|$)' -or
        $_.Defines -notmatch '(^|\s)(/D|-D)NDEBUG(\s|$)'
    }
    if ($missingCompilePolicy) {
        $message = ("{0} of {1} {2} object rules are not " +
            "Release-optimized (/O2 /Ob2 /DNDEBUG required)") -f
            $missingCompilePolicy.Count, $objectRules.Count, $Target
        throw $message
    }

    $linkFlags = $null
    $linkPattern = '^build ' + [regex]::Escape($Target) + '\.exe:'
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index] -notmatch $linkPattern) { continue }
        for ($ruleIndex = $index + 1;
             $ruleIndex -lt [Math]::Min($index + 20, $lines.Count);
             ++$ruleIndex) {
            if ($lines[$ruleIndex] -match '^  LINK_FLAGS = (.*)$') {
                $linkFlags = $Matches[1]
                break
            }
        }
        break
    }
    if ($null -eq $linkFlags) {
        throw "$Target executable link rule was not found in $ninjaPath"
    }
    foreach ($requiredFlag in @('/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF',
                                '/Brepro')) {
        if ($linkFlags -notmatch ('(^|\s)' + [regex]::Escape($requiredFlag) +
                                  '(\s|$)')) {
            throw "$Target link rule is missing $requiredFlag"
        }
    }
    return $objectRules.Count
}

$objectRuleCount = Test-ReleaseTargetPolicy -Target 'TheDarkness'
$settingsObjectRuleCount =
    Test-ReleaseTargetPolicy -Target 'TheDarknessSettings'

[pscustomobject]@{
    BuildDirectory = $resolvedBuild
    ObjectRules = $objectRuleCount
    SettingsObjectRules = $settingsObjectRuleCount
    CompilePolicy = '/O2 /Ob2 /DNDEBUG'
    LinkPolicy = '/INCREMENTAL:NO /OPT:REF /OPT:ICF /Brepro'
    Status = 'PASS'
}
