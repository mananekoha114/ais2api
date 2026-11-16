@echo off
rem Copyright (c) 2026 Mana Nekoha (mananekoha114)
rem SPDX-License-Identifier: MPL-2.0
rem One-click launcher at the root of the Windows runtime archive. It
rem initializes config.json and data\ beside the executable on first use, runs
rem setup when no browser is available, and then runs the service in this
rem window until Ctrl+C. --background starts it detached instead, and
rem --skip-setup skips running setup when checking browser readiness. Every
rem other argument (for example --port 18452) is passed to each command.
rem
rem The console switches to UTF-8 (code page 65001) so Chinese text displays on
rem GBK systems; the previous code page is restored on exit. Under 65001 cmd
rem misplaces lines and labels in batch files containing multi-byte text, so
rem this file stays ASCII and :say prints share\ais2api\start-messages.txt.
setlocal EnableExtensions DisableDelayedExpansion
set "PREVIOUS_CODE_PAGE="
for /f "tokens=2 delims=:" %%c in ('chcp') do set "PREVIOUS_CODE_PAGE=%%c"
chcp 65001 >nul
set "PROGRAM=%~dp0bin\ais2api.exe"
set "MESSAGES=%~dp0share\ais2api\start-messages.txt"
if exist "%PROGRAM%" goto found
call :say missing
echo   %PROGRAM%
goto failed
:found
set "MODE=serve"
set "SKIP_SETUP="
set "ARGUMENTS="
:arguments
if "%~1"=="" goto parsed
if /i "%~1"=="--background" (set "MODE=start") else if /i "%~1"=="--skip-setup" (set "SKIP_SETUP=1") else set "ARGUMENTS=%ARGUMENTS% %1"
shift
goto arguments
:parsed

rem Redirected output is single-line JSON. init refuses to run while the
rem service is running, which also answers whether it is already running.
set "INIT_RESULT=%TEMP%\ais2api-init-%RANDOM%%RANDOM%.json"
"%PROGRAM%" init %ARGUMENTS% >"%INIT_RESULT%" 2>&1
if not errorlevel 1 goto initialized
findstr /c:"\"reason\":\"already_running\"" "%INIT_RESULT%" >nul
if errorlevel 1 goto init_failed
del "%INIT_RESULT%" 2>nul
call :say running
call :commands
goto finished

:init_failed
del "%INIT_RESULT%" 2>nul
rem Repeat on the console so the error is shown in readable form.
"%PROGRAM%" init %ARGUMENTS% >nul
goto failed

:initialized
findstr /c:"\"api_key_configured\":false" "%INIT_RESULT%" >nul
if not errorlevel 1 call :say api_key
findstr /c:"\"admin_password_default\":true" "%INIT_RESULT%" >nul
if not errorlevel 1 call :say password
del "%INIT_RESULT%" 2>nul

if defined SKIP_SETUP goto browser_ready
"%PROGRAM%" doctor %ARGUMENTS% | findstr /c:"\"browser_exists\":true" >nul
if not errorlevel 1 goto browser_ready
call :say setup
"%PROGRAM%" setup %ARGUMENTS% >nul
if errorlevel 1 goto failed
call :say prepared
:browser_ready

if "%MODE%"=="serve" goto serve
"%PROGRAM%" start %ARGUMENTS% >nul
if errorlevel 1 goto failed
call :say background
call :commands
goto finished

:serve
call :say serve
"%PROGRAM%" serve %ARGUMENTS%
if errorlevel 1 goto failed
goto finished

:failed
rem Keep a double-clicked window open so the error stays readable.
pause
if defined PREVIOUS_CODE_PAGE chcp %PREVIOUS_CODE_PAGE% >nul
endlocal
exit /b 1

:finished
if defined PREVIOUS_CODE_PAGE chcp %PREVIOUS_CODE_PAGE% >nul
endlocal
exit /b 0

:commands
call :say status
echo   "%PROGRAM%" status
call :say stop
echo   "%PROGRAM%" stop
exit /b 0

rem Prints the "name|text" line of the message file, decoded as UTF-8.
:say
for /f "usebackq tokens=1,* delims=|" %%a in ("%MESSAGES%") do if "%%a"=="%~1" echo(%%b
exit /b 0
