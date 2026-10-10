@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Dev.ps1" -RunTests
set "build_result=%ERRORLEVEL%"
echo.
if "%build_result%"=="0" (
    echo BUILD AND TESTS SUCCEEDED. You can now open PortalDev.exe.
) else (
    echo BUILD OR TEST FAILED. Please send the first error shown above.
)
echo Press any key to close this window.
pause >nul
exit /b %build_result%
