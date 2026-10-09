[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dependency = Join-Path $repo 'third_party\III.VC.SA.WindowedMode'
$premake = Join-Path $dependency 'premake5.exe'
$solution = Join-Path $dependency 'build\III.VC.SA.WindowedMode.sln'
$builtAsi = Join-Path $dependency 'data\III.VC.SA.WindowedMode.asi'
$config = Join-Path $repo 'src\config\III.VC.SA.WindowedMode.ini'
$devScripts = Join-Path $repo 'game\dev\GTA San Andreas\scripts'

if (-not (Test-Path -LiteralPath $premake)) {
    throw 'WindowedMode is missing. Run: git submodule update --init --recursive'
}

$requiredDependencyFiles = @(
    (Join-Path $dependency 'external\IniReader\IniReader.h'),
    (Join-Path $dependency 'external\injector\include\injector\injector.hpp'),
    (Join-Path $dependency 'external\injector\safetyhook\include\safetyhook.hpp')
)
if (-not $SkipBuild -and ($requiredDependencyFiles | Where-Object { -not (Test-Path -LiteralPath $_) })) {
    throw 'WindowedMode nested dependencies are missing. Run: git submodule update --init --recursive'
}

$candidates = @(
    @{
        Toolset = 'v143'
        MSBuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
        RuntimeGlob = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*\lib\x86\libcpmt.lib'
    },
    @{
        Toolset = 'v145'
        MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
        RuntimeGlob = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\*\lib\x86\libcpmt.lib'
    }
)

if (-not $SkipBuild) {
    $toolchain = $null
    foreach ($candidate in $candidates) {
        if ((Test-Path -LiteralPath $candidate.MSBuild) -and
            (Get-Item -Path $candidate.RuntimeGlob -ErrorAction SilentlyContinue)) {
            $toolchain = $candidate
            break
        }
    }
    if (-not $toolchain) {
        throw 'No complete MSVC x86 toolchain was found. Install Desktop development with C++.'
    }

    Push-Location $dependency
    try {
        & $premake vs2022
        if ($LASTEXITCODE -ne 0) { throw "WindowedMode Premake failed with exit code $LASTEXITCODE" }

        & $toolchain.MSBuild $solution '/m' '/v:minimal' `
            '/p:Configuration=Release' '/p:Platform=Win32' `
            "/p:PlatformToolset=$($toolchain.Toolset)"
        if ($LASTEXITCODE -ne 0) { throw "WindowedMode build failed with exit code $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}

if (-not (Test-Path -LiteralPath $builtAsi)) {
    throw "WindowedMode ASI was not found: $builtAsi"
}
if (-not (Test-Path -LiteralPath $config)) {
    throw "WindowedMode configuration was not found: $config"
}
if (-not (Test-Path -LiteralPath $devScripts)) {
    throw "The dev game's scripts directory was not found: $devScripts"
}

Copy-Item -LiteralPath $builtAsi -Destination $devScripts -Force
Copy-Item -LiteralPath $config -Destination $devScripts -Force

# Compatibility settings are keyed by the full EXE path; the clean copy's DPI setting
# does not apply to the dev copy, even though their EXE hashes are identical.
$devGame = Join-Path (Split-Path -Parent $devScripts) 'gta-sa.EXE'
$layers = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
New-Item -Path $layers -Force | Out-Null
New-ItemProperty -Path $layers -Name $devGame -PropertyType String `
    -Value '~ HIGHDPIAWARE' -Force | Out-Null

Write-Host ''
Write-Host 'WindowedMode installed in the dev game only:'
Get-Item -LiteralPath `
    (Join-Path $devScripts 'III.VC.SA.WindowedMode.asi'), `
    (Join-Path $devScripts 'III.VC.SA.WindowedMode.ini') |
    Select-Object FullName, Length, LastWriteTime
Write-Host "Dev game DPI compatibility: $devGame = ~ HIGHDPIAWARE"
