@echo off
setlocal

set "BUILD_DIRECTORY=build-windows"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALLATION="
set "CMAKE_EXE=cmake"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALLATION=%%I"
    )
)

if defined VS_INSTALLATION (
    call "%VS_INSTALLATION%\VC\Auxiliary\Build\vcvars64.bat"
    if exist "%VS_INSTALLATION%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%VS_INSTALLATION%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)

if not exist "%CMAKE_EXE%" (
    where cmake >nul 2>nul
    if errorlevel 1 (
        echo Visual Studio C++ and CMake were not found.
        echo Install the Desktop development with C++ workload and run this file again.
        pause
        exit /b 1
    )
)

if exist "%BUILD_DIRECTORY%" (
    rmdir /s /q "%BUILD_DIRECTORY%"
)

"%CMAKE_EXE%" -S . -B "%BUILD_DIRECTORY%" -A x64
if errorlevel 1 (
    pause
    exit /b 1
)

"%CMAKE_EXE%" --build "%BUILD_DIRECTORY%" --config Release
if errorlevel 1 (
    pause
    exit /b 1
)

echo.
echo DragonByteZ GUI:
echo %CD%\%BUILD_DIRECTORY%\Release\DragonByteZ.exe
echo.
echo DragonByteZ command-line tool:
echo %CD%\%BUILD_DIRECTORY%\Release\dragonbytez-cli.exe

start "" "%CD%\%BUILD_DIRECTORY%\Release\DragonByteZ.exe"

endlocal
