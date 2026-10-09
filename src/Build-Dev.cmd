@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Dev.ps1"
set "build_result=%ERRORLEVEL%"
echo.
if "%build_result%"=="0" (
    echo BUILD SUCCEEDED. You can now open PortalDev.exe.
) else (
    echo BUILD FAILED. Please send the first error shown above.
)
echo Press any key to close this window.
pause >nul
exit /b %build_result%
