$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/_probe_input_observation.ps1')

function Assert-Observed([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function New-ObservationLine([int]$Sequence, [int]$Step, [string]$Buttons) {
    'REX_EMBEDDED_INPUT_TO_SWAP sequence={0} step={1} swap={2} refresh=1 input_qpc={3} swap_qpc={4} frequency=10000000 delta_us=1 packet={0} buttons=0x{5} state_hash=0x0 draws=1 resolves=1' -f (
        $Sequence, $Step, (100 + $Sequence + $Step),
        (9007199254740992L + $Sequence * 100),
        (9007199254740992L + $Sequence * 100 + $Step), $Buttons)
}

$unknown = Get-ProbeInputObservation ''
Assert-Observed ($null -eq $unknown.button_activity_observed) 'No log is not proof of no input.'
Assert-Observed $unknown.frame_alignment_requires_review 'Missing observer must retain the state gate.'
$neutralLine = New-ObservationLine 1 0 '0000'
$neutral = Get-ProbeInputObservation ($neutralLine + "`r`n" + (New-ObservationLine 1 1 '0000'))
Assert-Observed ($neutral.observed_state_count -eq 1) 'Eight swap reports must not be counted as eight input events.'
Assert-Observed ($neutral.button_activity_observed -eq $false) 'Initial neutral state is not a button press.'
Assert-Observed (-not $neutral.complete_polling_history) 'Bounded telemetry must not claim complete observation.'
Assert-Observed ($neutral.actor -eq 'UNKNOWN') 'Do not infer physical/controller/keyboard source.'
Assert-Observed ($neutral.records[0].input_qpc -eq 9007199254741092L) 'QPC integer precision was lost.'

$startLine = New-ObservationLine 2 0 '0010'
$releaseLine = New-ObservationLine 3 0 '0000'
$activity = Get-ProbeInputObservation ($neutralLine + "`n" + $startLine + "`n" + $releaseLine + "`n" + $releaseLine)
Assert-Observed $activity.button_activity_observed 'Start/release must invalidate an assumed no-input comparison.'
Assert-Observed ($activity.observed_state_count -eq 3) 'Duplicate log blocks must be deduplicated.'
Assert-Observed ($activity.button_changes_between_observed_states -eq 2) 'Preserve both observed button-state transitions.'
Assert-Observed $activity.frame_alignment_requires_review 'Input activity needs scene alignment review.'
Assert-Observed ($activity.records[1].buttons -eq '0x0010') 'Retain the exact button mask.'

$held = Get-ProbeInputObservation (New-ObservationLine 1 0 '1000')
Assert-Observed $held.button_activity_observed 'An initially held A state is observed activity, not an inferred new edge.'
$capped = Get-ProbeInputObservation ($neutralLine + "`n" + $startLine + "`n" + $releaseLine) -MaximumRecords 1
Assert-Observed ($capped.records.Count -eq 1 -and $capped.records_truncated) 'Returned event detail must stay bounded.'
Assert-Observed ($capped.nonzero_button_state_count -eq 1) 'Count activity outside the retained detail prefix.'
$conflict = Get-ProbeInputObservation ($startLine + "`n" + (New-ObservationLine 2 0 '1000'))
Assert-Observed ($null -eq $conflict.button_activity_observed -and $conflict.conflicting_duplicate_count -eq 1) 'Conflicting state observations must fail closed.'
$malformed = Get-ProbeInputObservation ('prefix ' + $startLine + "`n" + (New-ObservationLine 4 0 '10000'))
Assert-Observed ($malformed.observed_state_count -eq 0) 'Do not accept embedded text or out-of-range button masks.'
Write-Output 'Probe input observation behavior passed (read-only, bounded, no actor or absent-input inference).'
