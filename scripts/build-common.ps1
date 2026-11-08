# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Toolchain setup shared by build.ps1 and build-dev.ps1; dot-source it.
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Native command failed: $Program (exit $LASTEXITCODE)" }
}
# GitHub occasionally times out (HTTP 408) on large partial-clone fetches.
function Invoke-Retried([scriptblock]$Action, [scriptblock]$Cleanup = {}) {
    for ($attempt = 1; ; $attempt++) {
        try { & $Action; return }
        catch {
            if ($attempt -ge 3) { throw }
            Write-Warning "$($_.Exception.Message); retrying ($attempt/3)"
            & $Cleanup
            Start-Sleep -Seconds (15 * $attempt)
        }
    }
}
function Resolve-ProjectPath([string]$Project, [string]$Path) {
    if ([string]::IsNullOrEmpty($Path) -or [IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $Project $Path
}
function Enter-BuildTools {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or [string]::IsNullOrEmpty($env:INCLUDE)) {
        $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path -LiteralPath $locator)) { throw 'Install Visual Studio C++ build tools or use Developer PowerShell.' }
        $installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $installation) { throw 'Visual Studio x64 C++ tools were not found.' }
        & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    }
    foreach ($tool in @('cl.exe', 'git.exe', 'cmake.exe', 'ctest.exe', 'ninja.exe')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing build tool: $tool" }
    }
}
# Clones vcpkg at the manifest baseline when missing; an existing checkout
# must already be at it.
function Initialize-Vcpkg([string]$Project, [string]$VcpkgRoot) {
    $baseline = (Get-Content -Raw -LiteralPath (Join-Path $Project 'vcpkg.json') | ConvertFrom-Json).'builtin-baseline'
    if ($baseline -notmatch '^[0-9a-f]{40}$') { throw 'vcpkg builtin-baseline must be a full commit SHA.' }
    if (-not (Test-Path -LiteralPath $VcpkgRoot)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $VcpkgRoot) | Out-Null
        Invoke-Retried { Invoke-Checked git.exe @('clone', '--filter=blob:none', '--no-checkout', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot) } { if (Test-Path -LiteralPath $VcpkgRoot) { Remove-Item -Recurse -Force -LiteralPath $VcpkgRoot } }
        Invoke-Retried { Invoke-Checked git.exe @('-C', $VcpkgRoot, 'checkout', '--detach', $baseline) }
    }
    $revision = & git.exe -C $VcpkgRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $revision -ne $baseline) { throw 'VcpkgRoot must match the manifest baseline; existing checkout was left unchanged.' }
    if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
        Invoke-Checked (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
    }
}
