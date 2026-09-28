$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../../scripts/_isolated_probe_observers.ps1"
function Assert-Policy([bool]$value, [string]$message) {
    if (-not $value) { throw $message }
}
Assert-Policy (@(Get-IsolatedProbeObserverArguments None).Count -eq 0) 'Normal run must have no observer arguments'
Assert-Policy (@(Get-IsolatedProbeObserverArguments GpuQuery).Count -eq 0) 'GPU query mode must not enable guest, input or audio observers'
Assert-Policy ((@(Get-IsolatedProbeObserverArguments Cadence) -join ',') -eq '--frame-cadence-diagnostics') 'Cadence must exclude input and audio observers'
Assert-Policy ((@(Get-IsolatedProbeObserverArguments Full) -join ',') -eq '--input-diagnostics,--frame-cadence-diagnostics,--audio-diagnostics') 'Existing default must be preserved'
foreach ($name in @('REX_EMBEDDED_PC_SCENE_HISTORY','DARKNESS_SOURCE_MEMORY_TRACKING','DARKNESS_OWNED_CAMERA_CONTINUOUS','darkness_owned_camera_continuous')) {
    Assert-Policy (Test-IsolatedProbeEnvironmentName $name) "Inherited optional mode must be cleared: $name"
}
foreach ($name in @('PATH','TEMP','DARKNESS_UNRELATED','XREX_OTHER')) {
    Assert-Policy (-not (Test-IsolatedProbeEnvironmentName $name)) "Unrelated environment must be preserved: $name"
}
# Validate the real launcher's syntax and rejected combination before its process gate.
$script = "$PSScriptRoot/../../scripts/start-isolated-gameplay-probe.ps1"
$parseErrors = $null
$null = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$null, [ref]$parseErrors)
Assert-Policy ($parseErrors.Count -eq 0) 'Launcher syntax must parse'
$rejected = $false
try { & $script -RunName observer_policy_must_not_launch -ObserverMode None -TemporalDiagnostics -PrepareOnly }
catch { $rejected = $_.Exception.Message -eq 'ObserverMode None requires optional CP and temporal diagnostics off' }
Assert-Policy $rejected 'Reject misleading diagnostic-free label before creating or launching anything'
$rejected = $false
try { & $script -RunName observer_policy_must_not_launch -SourceTrackingCostProbe -ObserverMode None -PrepareOnly }
catch { $rejected = $_.Exception.Message -eq 'Source tracking cost probe requires cadence-only observation and no temporal or timing overrides' }
Assert-Policy $rejected 'Cost probe must not pretend to be an observer-free player run'
foreach ($limit in @(0,256)) {
    Assert-IsolatedProbeSubmissionPolicy -Mode GpuQuery -Version 276 -CpDiagnostics $true -ZpdGpuTiming $true -SubmitAfterDraws $limit
    Assert-IsolatedProbeSubmissionPolicy -Mode None -Version 276 -SubmitAfterDraws $limit
    # Existing GPU-only interval recorder may observe the unchanged batching mode.
    Assert-IsolatedProbeSubmissionPolicy -Mode None -Version 276 -SubmitAfterDraws $limit -SwapIntervalDiagnostics $true
}
$valid = @{ Mode='GpuQuery'; Version=276; CpDiagnostics=$true; ZpdGpuTiming=$true; SubmitAfterDraws=256 }
foreach ($override in @(
    @{Mode='None'}, @{Mode='Cadence'}, @{Mode='Full'}, @{Version=265},
    @{CpDiagnostics=$false}, @{ZpdGpuTiming=$false}, @{TemporalDiagnostics=$true},
    @{ZpdSubmitAfterDraws=1}, @{SwapIntervalDiagnostics=$true}
)) {
    $case = $valid.Clone()
    foreach ($key in $override.Keys) { $case[$key] = $override[$key] }
    $rejected = $false
    try { Assert-IsolatedProbeSubmissionPolicy @case } catch { $rejected = $true }
    Assert-Policy $rejected ('Reject unsupported submission observer combination: ' + ($override | ConvertTo-Json -Compress))
}
$rejected = $false
try { & $script -RunName observer_policy_must_not_launch -Version v276 -ObserverMode GpuQuery -CpDiagnostics -SubmitAfterDraws 256 -PrepareOnly }
catch { $rejected = $_.Exception.Message -eq 'GpuQuery mode requires V252+, CP and ZPD GPU timing only' }
Assert-Policy $rejected 'Actual launcher must reject incomplete query diagnostics before creating anything'
Write-Output 'PASS: observer modes, inherited environment sanitation, launcher syntax and early rejection'
