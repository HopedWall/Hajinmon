@echo off
rem Starts the recompiled DOA4 port.
rem  - game data is read from ..\game (mounted as game:\ and d:\)
rem  - the GPU is emulated by the Xenos plugin (rexgpu-xenos*.dll next to the exe)
rem  - the log is written to ..\build\run.log
rem  - on-demand frame captures: create the file ..\build\shots\capture.req
rem Usage: tools\run_doa4.cmd [config]   (config: relwithdebinfo [default], release, debug)

set "ROOT=%~dp0.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=relwithdebinfo"
set "EXE=%ROOT%\doa4\out\build\win-amd64-%CONFIG%\HajinmonGame.exe"

if not exist "%EXE%" (
    echo Executable not found: "%EXE%"
    echo Build the project first, see README.md.
    exit /b 1
)
if not exist "%ROOT%\game\default.xex" (
    echo Game files not found in "%ROOT%\game", see README.md.
    exit /b 1
)

set "DOA4_CAPTURE_DIR=%ROOT%\build\shots"
rem Options:
rem  - Default render target path (rtv): rov is much slower on integrated GPUs.
rem    The SDK still applies the blend factors to MIN/MAX in the shader, like
rem    Xenos does (PRESS START text).
rem  - The game renders at 1280x720: resolution_scale=2 draws at 2560x1440 and
rem    the 1920x1080 window scales it down (supersampled 1080p).
rem  - alloc_top_down: 4 KB-page memory is allocated top-down like on the
rem    Xbox 360. With blocks at low addresses (0x00010000...) the DOA4 animation
rem    scripts misbehave (Hayabusa distorted, crashes).
start "" "%EXE%" --game_data_root="%ROOT%\game" --gpu_plugin=xenos --alloc_top_down --log_guest_fps --resolution_scale=2 --window_width=1920 --window_height=1080 --log_file="%ROOT%\build\run.log" --log_level=debug --log_flush_interval=1
