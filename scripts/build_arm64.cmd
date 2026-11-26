@echo off
rem Configure, build and test QTacquisitionApp natively for Windows on ARM: Qt for MSVC ARM64 and Visual Studio's
rem compiler (preset msvc-arm64-release). Logs in build\logs\arm64\ : environment.txt, configure.txt, build.txt,
rem tests.txt ; summary in status.txt. The MinGW x64 build (scripts\build.cmd) stays as it is.
rem Usage: scripts\build_arm64.cmd [debug]   (default: release)
setlocal EnableExtensions EnableDelayedExpansion
rem Started by a double click (cmd /c): keep the window open at the end.
set "INTERACTIVE=0"
echo %cmdcmdline% | find /i "/c" >nul && set "INTERACTIVE=1"
cd /d "%~dp0.."
set "LOGS=build\logs\arm64"
if not exist "%LOGS%" mkdir "%LOGS%"
echo running > "%LOGS%\status.txt"
set "STEP=environment"
echo [0/3] Environnement ARM64 natif...
call "%~dp0arm64_env.cmd" || goto :failed
cd /d "%~dp0.."

set "PRESET=msvc-arm64-release"
if /i "%~1"=="debug" set "PRESET=msvc-arm64-debug"
set "STEP=configure"
echo [1/3] Configuration (%PRESET%)...
cmake --preset %PRESET% > "%LOGS%\configure.txt" 2>&1 || goto :failed
set "STEP=build"
echo [2/3] Compilation...
cmake --build --preset %PRESET% --parallel 8 > "%LOGS%\build.txt" 2>&1 || goto :failed
set "STEP=tests"
echo [3/3] Tests...
if /i "%PRESET%"=="msvc-arm64-release" (
    ctest --preset msvc-arm64-all > "%LOGS%\tests.txt" 2>&1 || goto :failed
)
echo ok > "%LOGS%\status.txt"
echo Termine : compilation et tests OK, en ARM64 natif.
if "%INTERACTIVE%"=="1" pause
exit /b 0

:failed
echo failed %STEP% > "%LOGS%\status.txt"
echo Echec a l'etape %STEP% : voir %LOGS%\%STEP%.txt
if "%INTERACTIVE%"=="1" pause
exit /b 1
