@echo off
setlocal

set CONFIG=Release
if not "%1"=="" set CONFIG=%1

set "CMAKE_BIN="

where cmake >NUL 2>&1
if not errorlevel 1 set "CMAKE_BIN=cmake"

if "%CMAKE_BIN%"=="" (
    if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKE_BIN=C:\Program Files\CMake\bin\cmake.exe"
)

if "%CMAKE_BIN%"=="" (
    if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" set "CMAKE_BIN=C:\Program Files (x86)\CMake\bin\cmake.exe"
)

if "%CMAKE_BIN%"=="" (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_BIN=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if "%CMAKE_BIN%"=="" (
    echo [ERROR] Could not find cmake executable.
    echo Install CMake or add it to PATH.
    exit /b 1
)

set "CMAKE_GENERATOR=Visual Studio 17 2022"
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -property catalog_productLineVersion`) do set "VS_MAJOR=%%V"
)

if "%VS_MAJOR%"=="18" set "CMAKE_GENERATOR=Visual Studio 18 2026"
if "%VS_MAJOR%"=="17" set "CMAKE_GENERATOR=Visual Studio 17 2022"

if exist build (
    rmdir /s /q build
)

echo [INFO] Using CMake generator: %CMAKE_GENERATOR%
"%CMAKE_BIN%" -S . -B build -G "%CMAKE_GENERATOR%"
if errorlevel 1 exit /b 1

"%CMAKE_BIN%" --build build --config %CONFIG%
if errorlevel 1 exit /b 1

set "PACKAGE_DIR=build\package\%CONFIG%"
set "JSON_SOURCE="

if exist "%USERPROFILE%\Saved Games\DCS\Scripts\DCS-BIOS\doc\json\*.json" set "JSON_SOURCE=%USERPROFILE%\Saved Games\DCS\Scripts\DCS-BIOS\doc\json"
if "%JSON_SOURCE%"=="" if exist "%USERPROFILE%\Saved Games\DCS.openbeta\Scripts\DCS-BIOS\doc\json\*.json" set "JSON_SOURCE=%USERPROFILE%\Saved Games\DCS.openbeta\Scripts\DCS-BIOS\doc\json"
if "%JSON_SOURCE%"=="" if exist "%USERPROFILE%\Saved Games\DCS.openalpha\Scripts\DCS-BIOS\doc\json\*.json" set "JSON_SOURCE=%USERPROFILE%\Saved Games\DCS.openalpha\Scripts\DCS-BIOS\doc\json"
if "%JSON_SOURCE%"=="" if exist "..\..\Scripts\DCS-BIOS\doc\json\*.json" set "JSON_SOURCE=..\..\Scripts\DCS-BIOS\doc\json"

if exist "%PACKAGE_DIR%" (
    rmdir /s /q "%PACKAGE_DIR%"
)

mkdir "%PACKAGE_DIR%"
if errorlevel 1 exit /b 1

copy /y "build\%CONFIG%\dcsbios-serial-bridge.exe" "%PACKAGE_DIR%\dcsbios-serial-bridge.exe" >NUL
if errorlevel 1 exit /b 1

if not "%JSON_SOURCE%"=="" (
    xcopy "%JSON_SOURCE%" "%PACKAGE_DIR%\json\" /E /I /Y >NUL
    if errorlevel 1 exit /b 1
) else (
    echo [WARN] JSON control database not found in Saved Games or repository doc folder.
)

echo [OK] Ready to run: %PACKAGE_DIR%\dcsbios-serial-bridge.exe
