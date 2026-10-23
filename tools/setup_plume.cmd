@echo off
rem Clones the dependencies of the experimental native renderer at pinned commits:
rem - Plume (https://github.com/renderbag/plume), the graphics library, into external\plume.
rem - DXC binaries (https://github.com/renderbag/dxc-bin), the shader compiler, into
rem   external\dxc-bin.
rem Usage: tools\setup_plume.cmd

setlocal
set "ROOT=%~dp0.."
set "PLUME=%ROOT%\external\plume"
set "COMMIT=0ce184b19270e49236afbb74052a6b267494b86b"
set "DXC=%ROOT%\external\dxc-bin"
set "DXC_COMMIT=737ac9f51c3d06f0cd1ee7e14dc065bafd52310d"

if not exist "%PLUME%\CMakeLists.txt" (
    echo Cloning Plume...
    git clone https://github.com/renderbag/plume "%PLUME%" || exit /b 1
)
git -C "%PLUME%" fetch --quiet origin || exit /b 1
git -C "%PLUME%" checkout --quiet %COMMIT% || exit /b 1
git -C "%PLUME%" submodule update --init --recursive || exit /b 1
echo Plume ready at %COMMIT%.

if not exist "%DXC%\inc\dxcapi.h" (
    echo Cloning DXC...
    git clone https://github.com/renderbag/dxc-bin "%DXC%" || exit /b 1
)
git -C "%DXC%" fetch --quiet origin || exit /b 1
git -C "%DXC%" checkout --quiet %DXC_COMMIT% || exit /b 1
echo DXC ready at %DXC_COMMIT%.
