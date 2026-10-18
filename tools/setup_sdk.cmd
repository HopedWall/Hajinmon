@echo off
rem Downloads, patches and builds the ReXGlue SDK used by this project.
rem  1. clones rexglue-sdk v0.10.0 (with submodules) into external\rexglue-sdk
rem  2. replaces git symlink placeholders with real files (Git for Windows)
rem  3. applies patches\rexglue-sdk-v0.10.0.patch (runtime fixes for DOA4)
rem  4. builds and installs the SDK (Debug, Release and RelWithDebInfo)
rem The install step registers the SDK in the CMake user package registry, so
rem the doa4 project finds it automatically.
rem Usage: tools\setup_sdk.cmd

setlocal
set "ROOT=%~dp0.."
set "SDK=%ROOT%\external\rexglue-sdk"
set "PATCH=%ROOT%\patches\rexglue-sdk-v0.10.0.patch"

if not exist "%SDK%\CMakeLists.txt" (
    echo [1/4] Cloning rexglue-sdk v0.10.0...
    git clone --depth 1 --branch v0.10.0 --recurse-submodules --shallow-submodules ^
        https://github.com/rexglue/rexglue-sdk "%SDK%" || exit /b 1
) else (
    echo [1/4] rexglue-sdk already cloned.
)

echo [2/4] Fixing git symlink placeholders...
py "%~dp0fix_git_symlinks.py" "%SDK%" || exit /b 1

echo [3/4] Applying the DOA4 patch...
git -C "%SDK%" apply --reverse --check --ignore-whitespace "%PATCH%" >nul 2>&1
if not errorlevel 1 (
    echo        already applied.
) else (
    git -C "%SDK%" apply --ignore-whitespace "%PATCH%" || exit /b 1
)

echo [4/4] Building the SDK (this takes a while)...
call "%~dp0rex_env.cmd" || exit /b 1
pushd "%SDK%"
rem Both graphics backends: Direct3D 12 (default) and Vulkan (--gpu_backend=vulkan).
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON || (popd & exit /b 1)
cmake --build out/build/win-amd64 --target install || (popd & exit /b 1)
popd
echo Done.
