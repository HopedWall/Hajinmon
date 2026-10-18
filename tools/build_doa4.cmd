@echo off
rem Builds the recompiled game (HajinmonGame.exe).
rem Requires the SDK (tools\setup_sdk.cmd) and the extracted game files in
rem game\: the recompiled code is generated from game\default.xex.
rem Usage: tools\build_doa4.cmd [config]   (config: relwithdebinfo [default], release, debug)
rem Builds use 6 parallel jobs by default: the generated sources are large and
rem need a lot of memory. Set JOBS to change it.

setlocal
set "ROOT=%~dp0.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=relwithdebinfo"
if not defined JOBS set "JOBS=6"

if not exist "%ROOT%\game\default.xex" (
    echo Game files not found in "%ROOT%\game", see README.md.
    exit /b 1
)

call "%~dp0rex_env.cmd" || exit /b 1
pushd "%ROOT%\doa4"
rem The recompiled code is not in the repository: generate it from the game's
rem default.xex before configuring, so CMake sees the generated sources.
if not exist "generated\default\sources.cmake" (
    echo Generating the recompiled code from game\default.xex...
    "%ROOT%\external\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe" codegen doa4_manifest.toml || (popd & exit /b 1)
)
cmake --preset win-amd64-%CONFIG% || (popd & exit /b 1)
cmake --build --preset win-amd64-%CONFIG% -j %JOBS% || (popd & exit /b 1)
popd
echo Done: doa4\out\build\win-amd64-%CONFIG%\HajinmonGame.exe
