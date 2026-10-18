@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title SteeringSync ATS + ETS2 - Compilation

echo ==========================================================
echo   SteeringSync - ATS + ETS2 (Windows x64)
echo ==========================================================
echo.

set "LOG=%~dp0compilation.log"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSVER="
set "VSROOT="
set "VSMAJOR="
set "GEN="
set "CMAKE=cmake"

if not exist "%VSWHERE%" (
  echo ERREUR: Visual Studio Installer / vswhere introuvable.
  echo Installer Visual Studio avec le support Desktop C++.
  goto :fail
)

for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do set "VSVER=%%V"
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%V"
if not defined VSVER (
  echo ERREUR: compilateur C++ Microsoft non trouve.
  echo Installer la charge de travail Developpement Desktop en C++.
  goto :fail
)
for /f "tokens=1 delims=." %%V in ("%VSVER%") do set "VSMAJOR=%%V"
if "%VSMAJOR%"=="18" set "GEN=Visual Studio 18 2026"
if "%VSMAJOR%"=="17" set "GEN=Visual Studio 17 2022"
if not defined GEN (
  echo ERREUR: Visual Studio non pris en charge: %VSVER%
  goto :fail
)

echo Visual Studio : %VSROOT%
echo Generateur : %GEN%
echo.

where cmake >nul 2>&1
if errorlevel 1 (
  if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  ) else (
    echo ERREUR: CMake introuvable. Installer CMake via Visual Studio Installer.
    goto :fail
  )
)

REM Nettoyer l'ancienne configuration (un ancien generateur peut rester en cache).
if exist "build" (
  echo Nettoyage du dossier build...
  rmdir /s /q "build"
  if exist "build" (
    echo ERREUR: impossible de nettoyer build. Fermer Visual Studio.
    goto :fail
  )
)

>"%LOG%" echo === SteeringSync - Configuration CMake ===
>>"%LOG%" echo Version Visual Studio: %VSVER%
>>"%LOG%" echo Generateur: %GEN%
>>"%LOG%" echo Repertoire projet: %CD%
>>"%LOG%" echo.

echo [1/2] Configuration CMake...
REM IMPORTANT : chemins relatifs, pas de CMAKE_GENERATOR_INSTANCE.
REM L'ancienne commande coupait le chemin Visual Studio aux espaces.
"%CMAKE%" -S . -B build -G "%GEN%" -A x64 >>"%LOG%" 2>&1
if errorlevel 1 goto :fail_log

echo [2/2] Compilation Release...
>>"%LOG%" echo.
>>"%LOG%" echo === Compilation Release ===
"%CMAKE%" --build build --config Release --parallel >>"%LOG%" 2>&1
if errorlevel 1 goto :fail_log

if not exist "build\output\Release\SteeringSync_Universal_ATS_1_61.dll" goto :missing
if not exist "build\output\Release\SteeringSync_Universal_ETS2_1_61.dll" goto :missing

echo.
echo [SUCCES] Les DLL sont dans :
echo %CD%\build\output\Release\
echo.
echo   SteeringSync_Universal_ATS_1_61.dll
echo   SteeringSync_Universal_ETS2_1_61.dll
goto :end

:missing
>>"%LOG%" echo ERREUR: compilation terminee, mais DLL manquantes.
goto :fail_log

:fail_log
echo.
echo [ECHEC] Journal compilation.log :
echo ----------------------------------------------------------
if exist "%LOG%" type "%LOG%"
echo ----------------------------------------------------------
echo Envoie compilation.log si la compilation echoue.
goto :end

:fail
echo.
echo [ECHEC] Impossible de lancer la compilation.

:end
echo.
pause
endlocal
