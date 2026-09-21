@echo off
rem ESP-AMS post-process launcher for Bambu Studio
rem Usage: place this .bat alongside esp_ams_postprocess.py
rem Bambu Studio calls: "C:\path\to\esp_ams_postprocess.bat" "C:\temp\file.gcode"
rem This .bat finds the Python file next to it, locates the .py, and runs it.

setlocal
set "SCRIPT_DIR=%~dp0"
set "PY_SCRIPT=%SCRIPT_DIR%esp_ams_postprocess.py"

rem Find Python executable
where python >nul 2>nul
if %ERRORLEVEL%==0 (
    python "%PY_SCRIPT" %*
    goto :EOF
)

rem Try py launcher
where py >nul 2>nul
if %ERRORLEVEL%==0 (
    py -3 "%PY_SCRIPT" %*
    goto :EOF
)

rem Try python from Bambu Studio bundled
for %%P in (
    "%APPDATA%\BambuStudio\python"
    "%LOCALAPPDATA%\BambuStudio\python"
) do (
    if exist "%%~P\python.exe" (
        "%%~P\python.exe" "%PY_SCRIPT" %*
        goto :EOF
    )
)

echo [ESP-AMS] ERROR: Python not found. Please install Python 3.8+ and add to PATH.
pause
exit /b 1
