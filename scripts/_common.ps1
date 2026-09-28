Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-CommandLine {
    param([Parameter(Mandatory)][string]$Command)
    Write-Host "> $Command"
}

function Assert-CommandAvailable {
    param([Parameter(Mandatory)][string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found. See docs/TOOLCHAIN.md."
    }
}

function Get-ProjectRoot {
    (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Find-VisualStudio {
    # Any Visual Studio 2022 or newer edition, or the Build Tools, with the
    # x64 C++ tools (vswhere ships with the Visual Studio installer).
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio 2022 (or its Build Tools) was not found (no vswhere.exe). See BUILDING.md.'
    }
    $installation = @(& $vswhere -latest -products '*' -version '[17.0,)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $installation.Count -or -not $installation[0]) {
        throw 'Visual Studio 2022 or newer with "Desktop development with C++" was not found. See BUILDING.md.'
    }
    return $installation[0]
}

function Initialize-DeveloperToolchain {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machinePath;$userPath"
    # VsDevCmd's extensions run vswhere from PATH; without it they print an
    # error that stops this script when its output is redirected.
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    if (Test-Path -LiteralPath $installer -PathType Container) { $env:Path = "$installer;$env:Path" }

    $visualStudio = Find-VisualStudio
    $vsDevCmd = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        throw "Visual Studio developer environment was not found: $vsDevCmd"
    }

    # Keep headers and static C++ runtime libraries on the same toolset. The
    # optimized objects use STL entry points introduced by 14.44; a 14.38 link
    # environment cannot resolve them and can leave a mixed incremental build.
    # DARKNESS_VCVARS_VER picks a toolset; otherwise 14.44 when installed, else
    # the newest one.
    $toolset = $env:DARKNESS_VCVARS_VER
    if (-not $toolset -and
        @(Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory `
            -Filter '14.44*' -ErrorAction SilentlyContinue).Count) {
        $toolset = '14.44'
    }
    $vcvarsArgument = ''
    if ($toolset) { $vcvarsArgument = " -vcvars_ver=$toolset" }
    cmd /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64$vcvarsArgument >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }

    $llvmBin = Join-Path $visualStudio 'VC\Tools\Llvm\x64\bin'
    if (-not (Test-Path -LiteralPath (Join-Path $llvmBin 'clang-cl.exe') -PathType Leaf)) {
        throw "x64 clang-cl was not found in $llvmBin (install the Visual Studio component 'C++ Clang tools for Windows')."
    }
    $env:Path = "$llvmBin;$env:Path"

    Assert-CommandAvailable cmake
    Assert-CommandAvailable ninja
    Assert-CommandAvailable clang-cl
}
