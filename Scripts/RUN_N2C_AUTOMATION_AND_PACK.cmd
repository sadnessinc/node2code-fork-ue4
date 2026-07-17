@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
title NodeToCode UE4.27 Automation

set "SCRIPT_DIR=%~dp0"
set "POWERSHELL_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
set "PAUSE_AT_END=1"
set "SYNTAX_SCRIPT=%SCRIPT_DIR%Codex\Test-N2CPowerShellSyntax.ps1"

:scan_args
if "%~1"=="" goto run_automation
if /I "%~1"=="-NoPause" set "PAUSE_AT_END=0"
shift
goto scan_args

:run_automation
if not exist "%POWERSHELL_EXE%" (
    echo.
    echo ========================================================================
    echo Error.
    echo Stopped at: startup
    echo Details: Windows PowerShell 5.1 was not found.
    echo Expected file: %POWERSHELL_EXE%
    echo ========================================================================
    set "EXIT_CODE=1"
    goto finish
)

if not exist "%SYNTAX_SCRIPT%" (
    echo.
    echo ========================================================================
    echo Error.
    echo Stopped at: startup
    echo Details: PowerShell syntax preflight script was not found.
    echo Expected file: %SYNTAX_SCRIPT%
    echo ========================================================================
    set "EXIT_CODE=1"
    goto finish
)

if not exist "%SCRIPT_DIR%Codex\Invoke-N2CFullValidation.ps1" (
    echo.
    echo ========================================================================
    echo Error.
    echo Stopped at: startup
    echo Details: Automation script was not found.
    echo Expected file: %SCRIPT_DIR%Codex\Invoke-N2CFullValidation.ps1
    echo ========================================================================
    set "EXIT_CODE=1"
    goto finish
)

echo [0/11] PowerShell syntax preflight
"%POWERSHELL_EXE%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SYNTAX_SCRIPT%" -ScriptsRoot "%SCRIPT_DIR%Codex"
if errorlevel 1 (
    echo.
    echo ========================================================================
    echo Error.
    echo Stopped at: PowerShell syntax preflight
    echo Details: A PowerShell script could not be parsed. See the parser error above.
    echo ========================================================================
    set "EXIT_CODE=1"
    goto finish
)

"%POWERSHELL_EXE%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Codex\Invoke-N2CFullValidation.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
if not defined EXIT_CODE set "EXIT_CODE=1"

:finish
if "%PAUSE_AT_END%"=="1" (
    echo.
    echo Press any key to exit.
    pause >nul
)
exit /b %EXIT_CODE%
