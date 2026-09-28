$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$evaluateTool = Join-Path $root 'scripts\evaluate-visual-regression.ps1'
$detectTool = Join-Path $root 'scripts\detect-visual-artifact.ps1'
$checkpointTool = Join-Path $root 'scripts\detect-visual-checkpoint.ps1'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'darkness_visual_regression_{0}' -f [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Write-TestFrame([string]$Path, [bool]$WithLocalizedSheet) {
    $bitmap = [Drawing.Bitmap]::new(320, 180)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([Drawing.Color]::FromArgb(255, 8, 10, 12))
            $graphics.FillRectangle(
                [Drawing.SolidBrush]::new(
                    [Drawing.Color]::FromArgb(255, 24, 35, 40)),
                24, 18, 272, 132)
            $graphics.FillEllipse(
                [Drawing.SolidBrush]::new(
                    [Drawing.Color]::FromArgb(255, 94, 78, 62)),
                126, 47, 70, 104)
            $graphics.DrawString(
                'THE DARKNESS',
                [Drawing.Font]::new('Arial', 15),
                [Drawing.Brushes]::White,
                [Drawing.PointF]::new(82, 20))
            if ($WithLocalizedSheet) {
                $graphics.FillPolygon(
                    [Drawing.SolidBrush]::new(
                        [Drawing.Color]::FromArgb(190, 205, 215, 205)),
                    [Drawing.Point[]]@(
                        [Drawing.Point]::new(108, 45),
                        [Drawing.Point]::new(260, 68),
                        [Drawing.Point]::new(245, 150),
                        [Drawing.Point]::new(120, 145)))
            }
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
}

function Write-CheckpointFrame([string]$Path, [bool]$WithPrompt) {
    $bitmap = [Drawing.Bitmap]::new(320, 180)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([Drawing.Color]::FromArgb(255, 7, 9, 11))
            $graphics.DrawString(
                'THE DARKNESS',
                [Drawing.Font]::new('Arial', 15),
                [Drawing.Brushes]::White,
                [Drawing.PointF]::new(82, 32))
            if ($WithPrompt) {
                $graphics.DrawString(
                    'PRESS START',
                    [Drawing.Font]::new('Consolas', 16),
                    [Drawing.Brushes]::White,
                    [Drawing.PointF]::new(90, 142))
            } else {
                $graphics.DrawString(
                    'ATTRACTION',
                    [Drawing.Font]::new('Consolas', 11),
                    [Drawing.Brushes]::DarkRed,
                    [Drawing.PointF]::new(225, 150))
            }
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
}

try {
    $reference = Join-Path $testRoot 'reference.png'
    $good = Join-Path $testRoot 'good.png'
    $defect = Join-Path $testRoot 'defect.png'
    $promptReference = Join-Path $testRoot 'prompt_reference.png'
    $prompt = Join-Path $testRoot 'prompt.png'
    $attraction = Join-Path $testRoot 'attraction.png'
    Write-TestFrame $reference $false
    Write-TestFrame $good $false
    Write-TestFrame $defect $true
    Write-CheckpointFrame $promptReference $true
    Write-CheckpointFrame $prompt $true
    Write-CheckpointFrame $attraction $false

    $manifestPath = Join-Path $testRoot 'manifest.json'
    [ordered]@{
        schema_version = 1
        reference_sets = [ordered]@{
            localized_scene = [ordered]@{
                classification = 'SYNTHETIC_POLICY_TEST_KNOWN_GOOD'
                animated_sequence = $false
                paths = @($reference)
                minimum_best_score = -2.0
                minimum_matching_frames = 1
                minimum_matching_fraction = 1.0
                maximum_invariant_failures = 0
                maximum_black_fraction_delta = 1.0
                maximum_saturated_red_fraction_delta = 1.0
                minimum_std_luma = 0.0
                maximum_mean_luma_delta = 1.0
                regions = @([ordered]@{
                    name = 'effect_region'
                    x = 0.30
                    y = 0.20
                    width = 0.58
                    height = 0.68
                    minimum_best_score = 0.95
                    maximum_black_fraction_delta = 0.05
                    maximum_saturated_red_fraction_delta = 1.0
                    minimum_std_luma = 0.01
                    maximum_mean_luma_delta = 0.05
                })
            }
        }
        artifact_reference_sets = [ordered]@{
            localized_sheet = [ordered]@{
                classification = 'SYNTHETIC_POLICY_TEST_KNOWN_BAD'
                paths = @($defect)
                minimum_best_score = 0.98
                minimum_matching_frames = 1
                regions = @([ordered]@{
                    name = 'effect_region'
                    x = 0.30
                    y = 0.20
                    width = 0.58
                    height = 0.68
                    minimum_best_score = 0.98
                })
            }
        }
        checkpoint_reference_sets = [ordered]@{
            press_start = [ordered]@{
                classification =
                    'GATE_DETECTION_ONLY_VISUAL_FIDELITY_NOT_ASSERTED'
                paths = @($promptReference)
                crop = [ordered]@{
                    x = 0.25
                    y = 0.68
                    width = 0.50
                    height = 0.31
                }
                sample_width = 128
                sample_height = 48
                bright_luma_threshold = 0.55
                bright_chroma_tolerance = 0.30
                maximum_mask_shift = 2
                minimum_best_score = 0.65
                minimum_matching_frames = 1
            }
        }
        profile_reference_sets = [ordered]@{}
    } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $manifestPath -Encoding UTF8

    $goodResult = & $evaluateTool -ManifestPath $manifestPath `
        -ReferenceSet localized_scene -ActualPath $good
    if (-not $goodResult.passed -or $goodResult.region_failures -ne 0) {
        throw 'Known-good localized visual frame did not pass.'
    }
    $defectResult = & $evaluateTool -ManifestPath $manifestPath `
        -ReferenceSet localized_scene -ActualPath $defect
    if ($defectResult.passed -or $defectResult.region_failures -lt 1) {
        throw 'Localized sheet was diluted by whole-frame evidence.'
    }

    $detectedResult = & $detectTool -ManifestPath $manifestPath `
        -ArtifactSet localized_sheet -ActualPath $defect
    if (-not $detectedResult.artifact_detected) {
        throw 'Known-bad localized sheet was not detected.'
    }
    $cleanResult = & $detectTool -ManifestPath $manifestPath `
        -ArtifactSet localized_sheet -ActualPath $good
    if ($cleanResult.artifact_detected -or $cleanResult.absence_proven) {
        throw 'Negative-reference miss made an unsupported correctness claim.'
    }
    $promptResult = & $checkpointTool -ManifestPath $manifestPath `
        -CheckpointSet press_start -ActualPath $prompt
    if (-not $promptResult.checkpoint_detected -or
        $promptResult.visual_fidelity_asserted) {
        throw 'Real checkpoint structure was not detected independently.'
    }
    $attractionResult = & $checkpointTool -ManifestPath $manifestPath `
        -CheckpointSet press_start -ActualPath $attraction
    if ($attractionResult.checkpoint_detected) {
        throw 'Attraction-only frame was mistaken for an input checkpoint.'
    }
    'Visual regression behavior passed'
} finally {
    if (Test-Path -LiteralPath $testRoot -PathType Container) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
