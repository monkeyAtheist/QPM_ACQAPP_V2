@echo off
rem Start QTacquisitionApp from the build tree, against the Qt it was built with (no deployment needed).
rem Messages (qWarning, QML errors) go to build\logs\app.txt.
rem Usage: scripts\run.cmd [debug]
setlocal EnableExtensions
cd /d "%~dp0.."
set "QT_ROOT=C:\Qt\Qt6.11.0"
set "PATH=%QT_ROOT%\6.11.0\mingw_64\bin;%QT_ROOT%\Tools\mingw1310_64\bin;%PATH%"
set "EXE=build\mingw-release\QTacquisitionApp.exe"
if /i "%~1"=="debug" set "EXE=build\mingw-debug\QTacquisitionApp.exe"
if not exist "%EXE%" (
    echo %EXE% introuvable : l'application arrive en phase 3 ; en phase 0 seuls le coeur de calcul et ses tests sont construits.
    pause
    exit /b 1
)
if not exist build\logs mkdir build\logs
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
