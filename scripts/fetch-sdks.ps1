[CmdletBinding()]
param(
    # The SDK licences are the vendors' own (NVIDIA RTX SDKs licence, AMD MIT,
    # Intel Simplified Software License). Pass this switch after reading them.
    [switch]$AcceptLicenses,
    [string[]]$Sdk = @('dlss', 'fsr', 'xess')
)
# Downloads the optional upscaler SDKs into build/deps from their official
# repositories at pinned commits. Sparse checkouts fetch only what the build
# uses: headers, import libraries, the runtime DLLs and the licences. Without
# an SDK the build simply leaves that option out. The NVIDIA DLSS SDK must
# never be committed to a public repository (its licence forbids it); build/
# is git-ignored.
. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
# "-Sdk fsr,xess" arrives as one string through powershell -File.
$Sdk = @($Sdk | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } |
    Where-Object { $_ })
foreach ($name in $Sdk) {
    if ($name -notin 'dlss', 'fsr', 'xess') { throw "Unknown SDK '$name' (use dlss, fsr, xess)." }
}
Assert-CommandAvailable git

$sdks = @{
    dlss = @{
        Name = 'NVIDIA DLSS SDK 310.9.1'
        Url = 'https://github.com/NVIDIA/DLSS.git'
        Commit = '374959484e79a640feaba44c93ac8cfb0a03f5b5'
        Folder = 'DLSS'
        Paths = @('/include/', '/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib',
                  '/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', '/lib/Windows_x86_64/rel/nvngx_dlss.dll',
                  '/LICENSE.txt')
        Dll = 'lib/Windows_x86_64/rel/nvngx_dlss.dll'
        Sha256 = '3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983'
        Licence = 'LICENSE.txt'
    }
    fsr = @{
        Name = 'AMD FidelityFX SDK (FSR 3.1), the ReXGlue fork of the official SDK'
        Url = 'https://github.com/rexglue/FidelityFX-SDK.git'
        Commit = 'eee08db1688ac3d1275a70b728f4a8ba22914213'
        Folder = 'FidelityFX-SDK'
        Paths = @('/ffx-api/include/', '/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll', '/LICENSE.txt')
        Dll = 'PrebuiltSignedDLL/amd_fidelityfx_dx12.dll'
        Sha256 = '12A5081257EC95B0B53AD51B4A87FB3C03F97FE0BBB59F9496968F8D50EF93A6'
        Licence = 'LICENSE.txt'
    }
    xess = @{
        Name = 'Intel XeSS SDK 2.0.2'
        Url = 'https://github.com/intel/xess.git'
        Commit = 'de0fb9c1c510661c571164e1418ceca8101dab69'
        Folder = 'XeSS'
        Paths = @('/inc/', '/lib/libxess.lib', '/bin/libxess.dll', '/LICENSE.txt',
                  '/third-party-programs.txt')
        Dll = 'bin/libxess.dll'
        Sha256 = '251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7'
        Licence = 'LICENSE.txt'
    }
}

if (-not $AcceptLicenses) {
    Write-Host 'The SDKs are licensed by their vendors:'
    foreach ($key in $Sdk) { Write-Host ("  {0}: {1}" -f $sdks[$key].Name, $sdks[$key].Url) }
    Write-Host 'Read their licences (LICENSE.txt in each repository), then run again with -AcceptLicenses.'
    exit 1
}

$deps = Join-Path $root 'build/deps'
New-Item -ItemType Directory -Force $deps | Out-Null
foreach ($key in $Sdk) {
    $entry = $sdks[$key]
    $target = Join-Path $deps $entry.Folder
    $dll = Join-Path $target $entry.Dll
    if ((Test-Path -LiteralPath $dll -PathType Leaf) -and
        (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash -eq $entry.Sha256) {
        Write-Host "$($entry.Name): already present"
        continue
    }
    if (Test-Path -LiteralPath $target) {
        throw "$target exists but is not the pinned $($entry.Name); move it away and run again."
    }
    Write-Host "$($entry.Name): $($entry.Url) @ $($entry.Commit)"
    $work = "$target.partial"
    if (Test-Path -LiteralPath $work) { throw "$work exists (an interrupted download); remove it and run again." }
    & git init --quiet $work
    if ($LASTEXITCODE -ne 0) { throw 'git init failed' }
    & git -C $work remote add origin $entry.Url
    & git -C $work config core.sparseCheckout true
    & git -C $work sparse-checkout set --no-cone @($entry.Paths)
    if ($LASTEXITCODE -ne 0) { throw 'git sparse-checkout failed (Git 2.25 or newer is needed)' }
    & git -C $work fetch --quiet --depth 1 --filter=blob:none origin $entry.Commit
    if ($LASTEXITCODE -ne 0) { throw "download of $($entry.Name) failed" }
    & git -C $work checkout --quiet FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "checkout of $($entry.Name) failed" }
    $fetched = Join-Path $work $entry.Dll
    if (-not (Test-Path -LiteralPath $fetched -PathType Leaf)) {
        throw "$($entry.Name): $($entry.Dll) is missing after the download"
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fetched).Hash
    if ($hash -ne $entry.Sha256) {
        throw "$($entry.Name): $($entry.Dll) SHA-256 $hash, expected $($entry.Sha256)"
    }
    Rename-Item -LiteralPath $work -NewName $entry.Folder
    Write-Host "$($entry.Name): ok ($($entry.Dll) verified)"
}
