@echo off
rem Start QTacquisitionApp built natively for Windows on ARM (scripts\build_arm64.cmd), against its Qt.
rem What changed since the last build is compiled first (the application only, without the tests).
rem Messages (qWarning, QML errors) go to build\logs\arm64\app.txt ; the compilation to build\logs\arm64\build_app.txt.
rem Usage: scripts\run_arm64.cmd [debug]
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0.."
set "LOGS=build\logs\arm64"
if not exist "%LOGS%" mkdir "%LOGS%"
set "PRESET=msvc-arm64-release"
if /i "%~1"=="debug" set "PRESET=msvc-arm64-debug"
set "EXE=build\%PRESET%\QTacquisitionApp.exe"
if not exist "build\%PRESET%\CMakeCache.txt" goto :not_built
call "%~dp0arm64_env.cmd" || goto :no_env
cd /d "%~dp0.."

echo Mise a jour de l'application...
cmake --build --preset %PRESET% --target QTacquisitionApp --parallel 8 > "%LOGS%\build_app.txt" 2>&1
if errorlevel 1 goto :build_failed
if not exist "%EXE%" goto :not_built

set QT_FORCE_STDERR_LOGGING=1
echo QTacquisitionApp ARM64 demarre, journal : %LOGS%\app.txt
"%EXE%" > "%LOGS%\app.txt" 2>&1
set "CODE=%ERRORLEVEL%"
if "%CODE%"=="0" exit /b 0
echo.
echo QTacquisitionApp s'est arrete avec le code %CODE%. Journal :
type "%LOGS%\app.txt"
pause
exit /b %CODE%

:not_built
echo Pas encore de compilation ARM64 : lancez d'abord scripts\build_arm64.cmd
pause
exit /b 1

:no_env
echo Environnement ARM64 incomplet : voir %LOGS%\environment.txt
pause
exit /b 1

:build_failed
echo.
echo La compilation a echoue. Journal : %LOGS%\build_app.txt
type "%LOGS%\build_app.txt"
pause
exit /b 1
