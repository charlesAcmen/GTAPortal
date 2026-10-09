[CmdletBinding()]
param(
    [switch]$SkipSdk,
    [switch]$RebuildSdk,
    [switch]$SkipTool
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $repo 'third_party\plugin-sdk\plugin-sdk'
$portalProject = Join-Path $repo 'src\PortalSA\PortalSA.vcxproj'
$toolProject = Join-Path $repo 'src\PortalDev\PortalDev.vcxproj'
$portalVProject = Join-Path $repo 'src\PortalV\PortalV.vcxproj'
$sdkProject = Join-Path $sdk 'plugin_sa\Plugin_SA.vcxproj'
$sdkLibrary = Join-Path $sdk 'output\lib\Plugin.lib'

if (-not (Test-Path -LiteralPath (Join-Path $sdk 'tools\premake\premake5.exe'))) {
    throw 'plugin-sdk is missing. Run: git submodule update --init --recursive'
}

$candidates = @(
    @{
        Action = 'vs2022'
        Toolset = 'v143'
        MSBuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
        RuntimeGlob = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\*\lib\x86\libcpmt.lib'
    },
    @{
        Action = 'vs2026'
        Toolset = 'v145'
        MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
        RuntimeGlob = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\*\lib\x86\libcpmt.lib'
    }
)

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

$env:PLUGIN_SDK_DIR = $sdk
Write-Host "Using $($toolchain.Toolset): $($toolchain.MSBuild)"

if (-not $SkipSdk -and ($RebuildSdk -or -not (Test-Path -LiteralPath $sdkProject) -or
    -not (Test-Path -LiteralPath $sdkLibrary))) {
    Push-Location $sdk
    try {
        & '.\tools\premake\premake5.exe' '--file=tools\premake\premake5.lua' "--pluginsdkdir=$sdk" $toolchain.Action
        if ($LASTEXITCODE -ne 0) { throw "Premake failed with exit code $LASTEXITCODE" }

        & $toolchain.MSBuild '.\plugin_sa\Plugin_SA.vcxproj' `
            '/p:Configuration=Release' '/p:Platform=Win32' '/m' '/v:minimal'
        if ($LASTEXITCODE -ne 0) { throw "Plugin_SA build failed with exit code $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}
elseif (-not $SkipSdk) {
    Write-Host "Reusing pinned plugin-sdk library: $sdkLibrary"
    Write-Host 'Use -RebuildSdk after changing or updating the submodule.'
}

& $toolchain.MSBuild $portalProject `
    '/p:Configuration=Release GTA-SA' '/p:Platform=Win32' `
    "/p:PlatformToolset=$($toolchain.Toolset)" '/m' '/v:minimal'
if ($LASTEXITCODE -ne 0) { throw "PortalSA build failed with exit code $LASTEXITCODE" }

if (-not $SkipTool) {
    & $toolchain.MSBuild $portalVProject `
        '/p:Configuration=Release' '/p:Platform=x64' `
        "/p:PlatformToolset=$($toolchain.Toolset)" '/m' '/v:minimal'
    if ($LASTEXITCODE -ne 0) { throw "PortalV build failed with exit code $LASTEXITCODE" }

    & $toolchain.MSBuild $toolProject `
        '/p:Configuration=Release' '/p:Platform=x64' `
        "/p:PlatformToolset=$($toolchain.Toolset)" '/m' '/v:minimal'
    if ($LASTEXITCODE -ne 0) { throw "PortalDev build failed with exit code $LASTEXITCODE" }
}

Write-Host ''
Write-Host 'Build complete:'
Get-Item -LiteralPath `
    (Join-Path $repo 'src\PortalSA\bin\GTA-SA\Release\PortalSA.asi'), `
    (Join-Path $repo 'src\PortalV\bin\x64\Release\PortalV.asi'), `
    (Join-Path $repo 'src\PortalDev\bin\x64\Release\PortalDev.exe') `
    -ErrorAction SilentlyContinue |
    Select-Object FullName, Length, LastWriteTime
