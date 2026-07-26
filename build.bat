@echo off
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: vswhere.exe not found at "%VSWHERE%".
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSPATH=%%i"
)

if not defined VSPATH (
    echo Error: Visual Studio C++ toolset build tools not found.
    exit /b 1
)

if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Error: vcvars64.bat not found at "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat".
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set "BUILD_DIR=%~dp0build"
set "SRC_DIR=%~dp0."

echo Attempting CMake configure with Ninja generator...
cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    echo Ninja generator failed. Falling back to Visual Studio 17 2022 generator...
    cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
    if %ERRORLEVEL% neq 0 (
        echo Error: CMake configuration failed.
        exit /b %ERRORLEVEL%
    )
)

echo Building Release target...
cmake --build "%BUILD_DIR%" --config Release
if %ERRORLEVEL% neq 0 (
    echo Error: CMake build failed.
    exit /b %ERRORLEVEL%
)

if exist "%BUILD_DIR%\UniPaste.exe" (
    echo Build succeeded: "%BUILD_DIR%\UniPaste.exe"
) else if exist "%BUILD_DIR%\Release\UniPaste.exe" (
    echo Build succeeded: "%BUILD_DIR%\Release\UniPaste.exe"
) else (
    echo Build finished, but UniPaste.exe executable location could not be confirmed.
)

exit /b 0
