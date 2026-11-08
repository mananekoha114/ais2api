# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$BrowserBundleDirectory = '',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidateSet('x64-windows-static', 'x64-windows-static-release')][string]$Triplet = 'x64-windows-static',
    [ValidateRange(1, 256)][int]$Jobs = 2,
    [switch]$Package,
    [switch]$WebUI,
    [switch]$SkipTests,
    [switch]$Help
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Help) {
    Write-Output 'Usage: ./scripts/build.ps1 [-BuildDirectory build/windows] [-VcpkgRoot PATH] [-BrowserBundleDirectory PATH] [-Configuration Release] [-Triplet x64-windows-static] [-Jobs 2] [-Package] [-WebUI] [-SkipTests]'
    Write-Output 'Requires Visual Studio C++ tools, Git and CMake/Ninja. New dependencies go under build/deps; existing .tools is not changed.'
    return
}
# The -release triplet skips debug dependency builds, so it cannot link a Debug (/MTd) configuration.
if ($Configuration -eq 'Debug' -and $Triplet.EndsWith('-release')) { throw "Triplet $Triplet has no debug libraries; use x64-windows-static for Debug." }
. (Join-Path $PSScriptRoot 'build-common.ps1')
$project = Split-Path -Parent $PSScriptRoot
$BuildDirectory = Resolve-ProjectPath $project $BuildDirectory
if ([string]::IsNullOrEmpty($VcpkgRoot)) { $VcpkgRoot = Join-Path $project 'build/deps/vcpkg' }
$VcpkgRoot = Resolve-ProjectPath $project $VcpkgRoot
$BrowserBundleDirectory = Resolve-ProjectPath $project $BrowserBundleDirectory
# Release archives never come from a dev build (scripts/build-dev.ps1).
$cache = Join-Path $BuildDirectory 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cache) -and (Select-String -LiteralPath $cache -Pattern '^AIS2API_DEV_BUNDLE:BOOL=ON$' -Quiet)) {
    throw "$BuildDirectory holds a dev build; use another -BuildDirectory."
}
Enter-BuildTools
Initialize-Vcpkg $project $VcpkgRoot
$testing = if ($SkipTests) { 'OFF' } else { 'ON' }
$webUiBuild = if ($WebUI) { 'ON' } else { 'OFF' }
Invoke-Checked cmake.exe @('-S', $project, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration", "-DBUILD_TESTING=$testing", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake", "-DVCPKG_TARGET_TRIPLET=$Triplet", "-DCMAKE_INSTALL_PREFIX=$BuildDirectory/install", "-DAIS2API_BROWSER_BUNDLE_DIR=$BrowserBundleDirectory", '-DAIS2API_DEV_BUNDLE=OFF', "-DAIS2API_BUILD_WEBUI=$webUiBuild")
Invoke-Checked cmake.exe @('--build', $BuildDirectory, '--parallel', "$Jobs")
if (-not $SkipTests) { Invoke-Checked ctest.exe @('--test-dir', $BuildDirectory, '--output-on-failure') }
Invoke-Checked cmake.exe @('--install', $BuildDirectory)
if ($Package) { Invoke-Checked cmake.exe @('--build', $BuildDirectory, '--target', 'package', '--parallel', "$Jobs") }
