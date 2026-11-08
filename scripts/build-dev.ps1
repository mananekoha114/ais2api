# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Builds a dev bundle for local use: the fpgen model embedded and the pinned
# Camoufox carried beside the executable. Release archives come from build.ps1.
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows-dev',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateSet('x64-windows-static', 'x64-windows-static-release')][string]$Triplet = 'x64-windows-static',
    [ValidateRange(1, 256)][int]$Jobs = 2,
    [string]$Proxy = '',
    [switch]$SkipTests,
    [switch]$SkipWebUI,
    [switch]$Help
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Help) {
    Write-Output 'Usage: ./scripts/build-dev.ps1 [-BuildDirectory build/windows-dev] [-VcpkgRoot PATH] [-Configuration Release] [-Triplet x64-windows-static] [-Jobs 2] [-Proxy URL] [-SkipTests] [-SkipWebUI]'
    Write-Output 'Downloads the pinned fpgen model and, through the built setup, the pinned Camoufox (about 1.3 GB, kept for later runs), then writes packages/*-Dev.zip.'
    Write-Output 'Requires Visual Studio C++ tools, Git, CMake/Ninja, Python 3 and, unless -SkipWebUI, Node.js.'
    return
}
# The -release triplet skips debug dependency builds, so it cannot link a Debug (/MTd) configuration.
if ($Configuration -eq 'Debug' -and $Triplet.EndsWith('-release')) { throw "Triplet $Triplet has no debug libraries; use x64-windows-static for Debug." }
. (Join-Path $PSScriptRoot 'build-common.ps1')
$project = Split-Path -Parent $PSScriptRoot
$BuildDirectory = Resolve-ProjectPath $project $BuildDirectory
if ([string]::IsNullOrEmpty($VcpkgRoot)) { $VcpkgRoot = Join-Path $project 'build/deps/vcpkg' }
$VcpkgRoot = Resolve-ProjectPath $project $VcpkgRoot
$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cache) -and -not (Select-String -LiteralPath $cache -Pattern '^AIS2API_DEV_BUNDLE:BOOL=ON$' -Quiet)) {
    throw "$BuildDirectory holds a release build; use another -BuildDirectory."
}
$python = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $python) { throw 'Python 3 is required to check the package.' }
$manifest = Get-Content -Raw -LiteralPath (Join-Path $project 'runtime-manifest.json') | ConvertFrom-Json
$platform = 'windows-x64'
$pin = $manifest.PSObject.Properties['camoufox']
if (-not $pin -or -not $pin.Value.artifacts.PSObject.Properties[$platform]) { throw "runtime-manifest.json pins no Camoufox for $platform." }
$bundle = Join-Path $BuildDirectory 'dev-bundle'
$data = Join-Path $bundle 'data'
$camoufox = Join-Path $data "runtime/camoufox/$($pin.Value.version)-$platform"
New-Item -ItemType Directory -Force -Path $bundle | Out-Null

# The fpgen model the build embeds, checked file by file.
$model = $manifest.fpgen_model
$modelDirectory = Join-Path $bundle ('fpgen/' + $model.tag.Replace('/', '-'))
function Test-Model {
    foreach ($file in $model.file_sha256.PSObject.Properties) {
        $path = Join-Path $modelDirectory $file.Name
        if (-not (Test-Path -LiteralPath $path) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() -ne $file.Value) { return $false }
    }
    return $true
}
if (-not (Test-Model)) {
    $archive = Join-Path $bundle 'fpgen-model.zip'
    $download = @('--fail', '--location', '--silent', '--show-error', '--connect-timeout', '20', '--output', $archive)
    if ($Proxy) { $download += @('--proxy', $Proxy) }
    Invoke-Retried { Invoke-Checked curl.exe ($download + @($model.url)) }
    if ((Get-Item -LiteralPath $archive).Length -ne $model.size -or (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLower() -ne $model.sha256) {
        throw 'The downloaded fpgen model does not match runtime-manifest.json.'
    }
    if (Test-Path -LiteralPath $modelDirectory) { Remove-Item -Recurse -Force -LiteralPath $modelDirectory }
    Expand-Archive -LiteralPath $archive -DestinationPath $modelDirectory
    Remove-Item -LiteralPath $archive
    if (-not (Test-Model)) { throw 'The fpgen model archive does not hold the pinned files.' }
}

Enter-BuildTools
Initialize-Vcpkg $project $VcpkgRoot
$testing = if ($SkipTests) { 'OFF' } else { 'ON' }
$webUiBuild = if ($SkipWebUI) { 'OFF' } else { 'ON' }
Invoke-Checked cmake.exe @('-S', $project, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", "-DBUILD_TESTING=$testing", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", "-DVCPKG_TARGET_TRIPLET=$Triplet", "-DCMAKE_INSTALL_PREFIX=$BuildDirectory/install", '-DAIS2API_DEV_BUNDLE=ON', "-DAIS2API_FPGEN_MODEL_DIR=$modelDirectory", "-DAIS2API_CAMOUFOX_BUNDLE_DIR=$camoufox", "-DAIS2API_BUILD_WEBUI=$webUiBuild")
Invoke-Checked cmake.exe @('--build', $BuildDirectory, '--parallel', "$Jobs")

# Runs the program with only the given ais2api variables set.
function Invoke-Program([string]$Program, [string[]]$Arguments, [hashtable]$Environment) {
    $names = @('AIS2API_DATA_DIR', 'AIS2API_BROWSER_BACKEND', 'AIS2API_BROWSER_EXECUTABLE', 'AIS2API_PROXY')
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    try {
        foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $Environment[$name], 'Process') }
        & $Program @Arguments
        if ($LASTEXITCODE -ne 0) { throw "Native command failed: $Program (exit $LASTEXITCODE)" }
    } finally {
        foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
}
# setup installs the pinned Camoufox, trimmed as every install is, and
# verifies it on a local page. A finished install is only verified again.
$environment = @{ AIS2API_DATA_DIR = $data; AIS2API_BROWSER_BACKEND = 'camoufox' }
if ($Proxy) { $environment.AIS2API_PROXY = $Proxy }
Invoke-Program (Join-Path $BuildDirectory 'ais2api.exe') @('setup', '--config', (Join-Path $bundle 'config.json')) $environment | Out-Null

if (-not $SkipTests) { Invoke-Checked ctest.exe @('--test-dir', $BuildDirectory, '--output-on-failure') }
$install = Join-Path $BuildDirectory 'install'
if (Test-Path -LiteralPath $install) { Remove-Item -Recurse -Force -LiteralPath $install }
Invoke-Checked cmake.exe @('--install', $BuildDirectory)

# A fresh configuration of the installed bundle needs no setup. doctor
# writes nothing there.
$smoke = Join-Path $bundle 'smoke'
$report = Invoke-Program (Join-Path $install 'bin/ais2api.exe') @('doctor', '--config', (Join-Path $smoke 'config.json')) @{ AIS2API_DATA_DIR = (Join-Path $smoke 'data') }
$doctor = $report | ConvertFrom-Json
if ($doctor.build_mode -ne 'dev' -or $doctor.browser_backend -ne 'camoufox' -or $doctor.camoufox.source -ne 'bundle' -or
    $doctor.fpgen_model.source -ne 'embedded' -or -not $doctor.browser_exists) {
    throw "The installed dev bundle is not ready to run: $report"
}

Invoke-Checked cmake.exe @('--build', $BuildDirectory, '--target', 'package', '--parallel', "$Jobs")
$packages = @(Get-ChildItem -Path (Join-Path $BuildDirectory 'packages') -Filter '*-Dev.zip' | Sort-Object LastWriteTime | Select-Object -Last 1)
if (-not $packages) { throw 'CPack wrote no dev package.' }
Invoke-Checked $python.Source @((Join-Path $PSScriptRoot 'check-package.py'), '--dev', $packages[0].FullName)
Write-Output "Dev bundle: $($packages[0].FullName)"
