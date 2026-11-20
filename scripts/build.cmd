@echo off
rem Configure, build and test QTacquisitionApp with Qt 6.11 / MinGW 13.1 (preset mingw-release).
rem Logs: build\logs\configure.txt, build.txt, tests.txt ; summary in build\logs\status.txt.
rem Usage: scripts\build.cmd [debug]   (default: release)
setlocal EnableExtensions
rem Started by a double click (cmd /c): keep the window open at the end.
set "INTERACTIVE=0"
echo %cmdcmdline% | find /i "/c" >nul && set "INTERACTIVE=1"
cd /d "%~dp0.."

set "QT_ROOT=C:\Qt\Qt6.11.0"
set "PRESET=mingw-release"
if /i "%~1"=="debug" set "PRESET=mingw-debug"
set "PATH=%QT_ROOT%\Tools\mingw1310_64\bin;%QT_ROOT%\6.11.0\mingw_64\bin;%PATH%"

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE if exist "%QT_ROOT%\Tools\CMake_64\bin\cmake.exe" set "CMAKE_EXE=%QT_ROOT%\Tools\CMake_64\bin\cmake.exe"
if not defined CMAKE_EXE (
    echo cmake introuvable : installez CMake ou ajoutez-le au PATH.
    if "%INTERACTIVE%"=="1" pause
    exit /b 1
)
for %%I in ("%CMAKE_EXE%") do set "CTEST_EXE=%%~dpIctest.exe"

if not exist build\logs mkdir build\logs
echo running > build\logs\status.txt
set "STEP=configure"
echo [1/3] Configuration (%PRESET%)...
"%CMAKE_EXE%" --preset %PRESET% > build\logs\configure.txt 2>&1 || goto :failed
set "STEP=build"
echo [2/3] Compilation...
"%CMAKE_EXE%" --build --preset %PRESET% --parallel 8 > build\logs\build.txt 2>&1 || goto :failed
set "STEP=tests"
echo [3/3] Tests...
if /i "%PRESET%"=="mingw-release" (
    "%CTEST_EXE%" --preset mingw-all > build\logs\tests.txt 2>&1 || goto :failed
)
echo ok > build\logs\status.txt
echo Termine : compilation et tests OK.
if "%INTERACTIVE%"=="1" pause
exit /b 0

:failed
echo failed %STEP% > build\logs\status.txt
echo Echec a l'etape %STEP% : voir build\logs\%STEP%.txt
if "%INTERACTIVE%"=="1" pause
exit /b 1
