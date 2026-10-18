@echo off
rem Sets up the build environment for ReXGlue:
rem  - loads the Visual Studio environment (MSVC headers, Windows SDK, linker, CMake, Ninja)
rem  - puts the official LLVM/Clang first in PATH (the one bundled with Visual Studio is too old)
rem Usage:  tools\rex_env.cmd            (from a cmd prompt, the environment stays active)
rem         cmd /c "tools\rex_env.cmd && <command>"
rem Set LLVM_DIR beforehand if LLVM is not installed in C:\Program Files\LLVM.

if not defined LLVM_DIR set "LLVM_DIR=C:\Program Files\LLVM"
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%i"

if not defined VS_DIR (
    echo Visual Studio with the C++ tools was not found.
    exit /b 1
)
if not exist "%LLVM_DIR%\bin\clang.exe" (
    echo LLVM not found in "%LLVM_DIR%".
    exit /b 1
)

rem some internal Visual Studio scripts look for vswhere.exe in PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%LLVM_DIR%\bin;%PATH%"
exit /b 0
