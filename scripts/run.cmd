@echo off
rem Start QTacquisitionApp from the build tree, against the Qt it was built with (no deployment needed).
rem What changed since the last build is compiled first (the application only, without the tests:
rem a few seconds when nothing changed), so the program started is always that of the sources.
rem Messages (qWarning, QML errors) go to build\logs\app.txt ; the compilation to build\logs\build_app.txt.
rem Usage: scripts\run.cmd [debug]
setlocal EnableExtensions
cd /d "%~dp0.."
set "QT_ROOT=C:\Qt\Qt6.11.0"
set "PRESET=mingw-release"
if /i "%~1"=="debug" set "PRESET=mingw-debug"
set "PATH=%QT_ROOT%\6.11.0\mingw_64\bin;%QT_ROOT%\Tools\mingw1310_64\bin;%PATH%"
set "EXE=build\%PRESET%\QTacquisitionApp.exe"
if not exist "build\%PRESET%\CMakeCache.txt" (
    echo Pas encore de compilation : lancez d'abord scripts\build.cmd
    pause
    exit /b 1
)

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE if exist "%QT_ROOT%\Tools\CMake_64\bin\cmake.exe" set "CMAKE_EXE=%QT_ROOT%\Tools\CMake_64\bin\cmake.exe"
if not exist build\logs mkdir build\logs
if defined CMAKE_EXE (
    echo Mise a jour de l'application...
    "%CMAKE_EXE%" --build --preset %PRESET% --target QTacquisitionApp --parallel 8 > build\logs\build_app.txt 2>&1
    if errorlevel 1 (
        echo.
        echo La compilation a echoue. Journal : build\logs\build_app.txt
        type build\logs\build_app.txt
        pause
        exit /b 1
    )
) else (
    echo cmake introuvable : l'application est lancee telle qu'elle a ete compilee.
)
if not exist "%EXE%" (
    echo %EXE% introuvable : lancez d'abord scripts\build.cmd
    pause
    exit /b 1
)

set QT_FORCE_STDERR_LOGGING=1
echo QTacquisitionApp demarre (journal : build\logs\app.txt)
"%EXE%" > build\logs\app.txt 2>&1
set "CODE=%ERRORLEVEL%"
if not "%CODE%"=="0" (
    echo.
    echo QTacquisitionApp s'est arrete avec le code %CODE%. Journal :
    type build\logs\app.txt
    pause
)
exit /b %CODE%
