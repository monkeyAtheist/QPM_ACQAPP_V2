@echo off
rem Native Windows on ARM environment, for scripts\build_arm64.cmd and scripts\run_arm64.cmd (they call it):
rem Visual Studio's ARM64 compiler (vcvarsall arm64), Qt for MSVC ARM64 (QACQ_QT_DIR ; its bin on the PATH),
rem cmake and ninja (Visual Studio's, else those of the Qt installation), the MSVC runtime of Visual Studio.
rem What it finds goes to build\logs\arm64\environment.txt.
rem Another Qt: set QACQ_QT_DIR=C:\QT_ARM\6.12.1\msvc2022_arm64 before starting the script.
rem No setlocal here: what it sets is for the script that calls it (which enables delayed expansion).
rem (From the project's folder, wherever it is started from: double-clicked, it starts in scripts\.)
cd /d "%~dp0.."
set "ENVLOG=build\logs\arm64\environment.txt"
if not exist build\logs\arm64 mkdir build\logs\arm64
echo Environnement ARM64 natif, %DATE% %TIME% > "%ENVLOG%"
echo Processeur : %PROCESSOR_ARCHITECTURE% >> "%ENVLOG%"

rem 1. Qt: the newest msvc*_arm64 under C:\Qt*\<version>\ or C:\Qt*\<installation>\<version>\ (C:\Qt, C:\QT_ARM...).
if defined QACQ_QT_DIR goto :qt_given
for /d %%Q in ("C:\Qt*") do (
    for /d %%V in ("%%~Q\6.*") do for /d %%K in ("%%~V\msvc*_arm64") do if exist "%%~K\bin\Qt6Core.dll" set "QACQ_QT_DIR=%%~K"
    for /d %%I in ("%%~Q\*") do for /d %%V in ("%%~I\6.*") do for /d %%K in ("%%~V\msvc*_arm64") do if exist "%%~K\bin\Qt6Core.dll" set "QACQ_QT_DIR=%%~K"
)
:qt_given
if not defined QACQ_QT_DIR goto :no_qt
if not exist "%QACQ_QT_DIR%\bin\Qt6Core.dll" goto :no_qt
echo Qt : %QACQ_QT_DIR% >> "%ENVLOG%"
set "QACQ_MISSING="
if not exist "%QACQ_QT_DIR%\lib\cmake\Qt6Multimedia\Qt6MultimediaConfig.cmake" set "QACQ_MISSING=Qt Multimedia"
if not exist "%QACQ_QT_DIR%\lib\cmake\Qt6SerialPort\Qt6SerialPortConfig.cmake" set "QACQ_MISSING=%QACQ_MISSING% Qt Serial Port"
if defined QACQ_MISSING goto :no_modules
for %%R in ("%QACQ_QT_DIR%\..\..") do set "QACQ_QT_ROOT=%%~fR"
rem (For CMake, with forward slashes: CMakeUserPresets.json reads it.)
set "QACQ_QT_CMAKE=%QACQ_QT_DIR:\=/%"

rem 2. Visual Studio 2022 or 2026 (or its Build Tools) with the ARM64 C++ tools, set up for a native ARM64 build (Qt's
rem    msvc2022_arm64 libraries work with the compilers of both).
set "QACQ_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%QACQ_VSWHERE%" goto :no_vs
set "QACQ_VS="
for /f "usebackq delims=" %%I in (`"%QACQ_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "QACQ_VS=%%I"
if not defined QACQ_VS for /f "usebackq delims=" %%I in (`"%QACQ_VSWHERE%" -latest -products * -property installationPath`) do set "QACQ_VS=%%I"
if not defined QACQ_VS goto :no_vs
if not exist "%QACQ_VS%\VC\Auxiliary\Build\vcvarsall.bat" goto :no_vs
echo Visual Studio : %QACQ_VS% >> "%ENVLOG%"
rem (Without it, the developer prompt moves to %USERPROFILE%\source when that folder exists.)
set "VSCMD_START_DIR=%CD%"
call "%QACQ_VS%\VC\Auxiliary\Build\vcvarsall.bat" arm64 > build\logs\arm64\vcvars.txt 2>&1
where cl > nul 2>&1 || goto :no_vs
cl 2>&1 | findstr /i /c:"version" >> "%ENVLOG%"

rem 3. cmake and ninja: those of Visual Studio if it has them, else those of the Qt installation.
where cmake > nul 2>&1 || for /d %%C in ("%QACQ_QT_ROOT%\Tools\CMake*") do if exist "%%~C\bin\cmake.exe" set "PATH=%%~C\bin;!PATH!"
where ninja > nul 2>&1 || for /d %%N in ("%QACQ_QT_ROOT%\Tools\Ninja*") do if exist "%%~N\ninja.exe" set "PATH=%%~N;!PATH!"
where cmake > nul 2>&1 || goto :no_cmake
where ninja > nul 2>&1 || goto :no_cmake
for /f "delims=" %%P in ('where cmake') do echo cmake : %%P >> "%ENVLOG%"
for /f "delims=" %%P in ('where ninja') do echo ninja : %%P >> "%ENVLOG%"
cmake --version | findstr /i /c:"version" >> "%ENVLOG%"

rem 4. To run: the Qt libraries, and the MSVC runtime of Visual Studio (when Windows has none for ARM64).
set "PATH=%QACQ_QT_DIR%\bin;!PATH!"
if defined VCToolsRedistDir for /d %%D in ("%VCToolsRedistDir%arm64\Microsoft.VC*.CRT") do set "PATH=!PATH!;%%~D"
if defined VCToolsRedistDir for /d %%D in ("%VCToolsRedistDir%debug_nonredist\arm64\Microsoft.VC*.DebugCRT") do set "PATH=!PATH!;%%~D"
echo Environnement pret >> "%ENVLOG%"
exit /b 0

:no_qt
echo Qt pour MSVC ARM64 introuvable sous C:\Qt* (C:\Qt, C:\QT_ARM...).
echo Installez avec l'installateur ARM64 de Qt : Qt 6.12, MSVC 2022 ARM64, Qt Multimedia, Qt Serial Port.
echo Ou indiquez son dossier avant : set QACQ_QT_DIR=C:\QT_ARM\6.12.1\msvc2022_arm64
echo Qt MSVC ARM64 introuvable >> "%ENVLOG%"
exit /b 1

:no_modules
echo Il manque a ce Qt : %QACQ_MISSING%.
echo Ajoutez-les avec le MaintenanceTool de Qt, rubrique Additional Libraries de Qt 6.12.
echo Qt %QACQ_QT_DIR% sans %QACQ_MISSING% >> "%ENVLOG%"
exit /b 1

:no_vs
echo Compilateur MSVC pour ARM64 introuvable.
echo Installez Visual Studio 2022 ou 2026, ou leurs Build Tools, avec la charge de travail Developpement Desktop en C++
echo et le composant des outils de build C++ ARM64/ARM64EC. Detail : build\logs\arm64\vcvars.txt
echo Pas de compilateur MSVC ARM64 >> "%ENVLOG%"
exit /b 1

:no_cmake
echo cmake ou ninja introuvable : ajoutez CMake et Ninja avec le MaintenanceTool de Qt
echo (Developer and Designer Tools), ou le composant Outils CMake C++ pour Windows de Visual Studio.
echo Pas de cmake ou de ninja >> "%ENVLOG%"
exit /b 1
