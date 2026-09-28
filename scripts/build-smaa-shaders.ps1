[CmdletBinding()]
param()
# Compiles the SMAA compute passes (external/ReXGlue/src/graphics/shaders/
# smaa.cs.hlsl, reference in shaders/smaa/) into the D3D12 bytecode headers
# the GPU plugin embeds: smaa_edge_detection_cs.h, smaa_blending_weight_cs.h,
# smaa_neighborhood_blending_cs.h (shaders::smaa_*_cs). Uses the pinned
# Windows SDK FXC like the other embedded shaders.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe'
if (!(Test-Path -LiteralPath $fxc)) { throw 'Pinned Windows SDK FXC unavailable' }
$source = Join-Path $root 'external/ReXGlue/src/graphics/shaders/smaa.cs.hlsl'
$out = Join-Path $root 'external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1'
foreach ($pass in @(@('smaa_edge_detection_cs', 0), @('smaa_blending_weight_cs', 1),
                    @('smaa_neighborhood_blending_cs', 2))) {
    $name = $pass[0]
    & $fxc /nologo /T cs_5_1 /E main /O3 /D "SMAA_PASS=$($pass[1])" /Vn $name /Fh (Join-Path $out "$name.h") $source
    if ($LASTEXITCODE -ne 0) { throw "FXC failed for $name" }
}
