@echo off
rem Copyright (c) 2026 Mana Nekoha (mananekoha114)
rem SPDX-License-Identifier: MPL-2.0
rem
rem Builds SpiderMonkey for ais2api on windows-x64: runs build-windows.sh in
rem the MozillaBuild shell with every download, cache and toolchain below the
rem work directory (a short path without spaces, such as E:\sm).
rem
rem Usage: build-windows.cmd <work directory>
rem MozillaBuild must be installed in <work directory>\mozilla-build.
setlocal
if "%~1"=="" (
  echo usage: build-windows.cmd ^<work directory^>
  exit /b 2
)
set "WORK=%~f1"
set "MOZILLABUILD=%WORK%\mozilla-build\"
if not exist "%MOZILLABUILD%msys2\msys2_shell.cmd" (
  echo MozillaBuild is not installed in %MOZILLABUILD%
  exit /b 1
)
set "MOZBUILD_STATE_PATH=%WORK%\mozbuild"
set "CARGO_HOME=%WORK%\cargo"
set "RUSTUP_HOME=%WORK%\rustup"
set "PIP_CACHE_DIR=%WORK%\pip-cache"
set "HOME=%WORK%\home"
set "TEMP=%WORK%\tmp"
set "TMP=%WORK%\tmp"
set "USE_MINTTY=0"
set "MSYSTEM="
set "MSYS="
set "CHERE_INVOKING=1"
rem Only the system's own directories: tools on the user's PATH must not take
rem the place of the ones the build bootstraps.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"
for %%D in ("%HOME%" "%TEMP%" "%MOZBUILD_STATE_PATH%") do if not exist "%%~D" mkdir "%%~D"
call "%MOZILLABUILD%msys2\msys2_shell.cmd" -no-start -defterm -full-path -c "sh \"$(cygpath -u '%~dp0build-windows.sh')\" '%WORK%'"
exit /b %ERRORLEVEL%
