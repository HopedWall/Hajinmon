@echo off
rem Clones Plume (https://github.com/renderbag/plume), the graphics library of the
rem experimental native renderer, into external\plume at a pinned commit.
rem Usage: tools\setup_plume.cmd

setlocal
set "ROOT=%~dp0.."
set "PLUME=%ROOT%\external\plume"
set "COMMIT=0ce184b19270e49236afbb74052a6b267494b86b"

if not exist "%PLUME%\CMakeLists.txt" (
    echo Cloning Plume...
    git clone https://github.com/renderbag/plume "%PLUME%" || exit /b 1
)
git -C "%PLUME%" fetch --quiet origin || exit /b 1
git -C "%PLUME%" checkout --quiet %COMMIT% || exit /b 1
git -C "%PLUME%" submodule update --init --recursive || exit /b 1
echo Plume ready at %COMMIT%.
