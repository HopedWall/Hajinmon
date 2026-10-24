# Hajinmon

**Dead or Alive 4, now on PC.**

Hajinmon is a native Windows port of the Xbox 360 version of *Dead or Alive 4*
(Tecmo / Team Ninja, 2005), built with **static recompilation**: the game's
PowerPC code is translated ahead of time into C++ and compiled for x86-64. It
runs on top of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which
provides the Xbox 360 kernel, the GPU (Direct3D 12 or Vulkan) and the audio
runtime.

> [!IMPORTANT]
> Neither this repository nor the releases contain any **game data**. You need
> your own copy of the game (an ISO dumped from the original Xbox 360 disc).

## Status

- Boots, plays the intro videos, title screen, menus and fights.
- 60 FPS on a dedicated GPU, rendered at 2x (2560x1440) and shown in a
  1920x1080 window by default.
- Two renderers: Direct3D 12 (default) and Vulkan.
- Experimental native renderer (Plume): draws the game with its shaders
  translated to HLSL instead of emulating the Xbox 360 GPU, with MSAA. Opt-in,
  in the launcher's *Graphics* tab on Windows and with `--renderer=plume` on
  Linux (see [Native renderer](#native-renderer)).
- Xbox controllers and keyboard/mouse work out of the box.
- Not everything has been tested yet. See [Troubleshooting](#troubleshooting)
  for how to collect information about crashes and graphical issues.

Tested with **Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It)**, title ID
`544307D1`.

## Play

1. Download `Hajinmon-<version>-x86_64.exe` from the
   [latest release](https://github.com/HopedWall/Hajinmon/releases/latest).
2. Start it. On the first start it asks for your Dead or Alive 4 disc image
   (`.iso`); you can change it later in the *Game* tab.
3. Adjust the settings if you like (see [Launcher](#launcher)) and press
   **Play**.

Nothing needs to be installed: the launcher contains the game runtime and
unpacks it on the first launch. It needs Windows 10/11 x64 and a Direct3D 12
or Vulkan GPU; a dedicated GPU is strongly recommended (integrated GPUs run the 3D scenes
below 60 FPS). The first start of the game takes longer while shaders are
compiled.

The launcher keeps its settings, save backups and the unpacked runtime in
`%LOCALAPPDATA%\Hajinmon`. The game's saves and shader cache are in
`Documents\Hajinmon` unless a different save folder is set in the launcher
(saves from earlier builds in `Documents\doa4` are moved there automatically).

## Building from source

The rest of this section is only needed to work on the project. The
recompiled code is not part of the repository: it is generated from your own
copy of the game during the build.

### Requirements

- Windows 10/11 x64.
- A Direct3D 12 GPU. A dedicated GPU is strongly recommended: integrated GPUs
  run the 3D scenes below 60 FPS.
- 16 GB of RAM recommended (the generated code is large to compile).
- About 30 GB of free disk space (ISO, extracted files, SDK and builds).
- [Git](https://git-scm.com/download/win).
- [Python 3.10+](https://www.python.org/downloads/) (the `py` launcher is used
  by the scripts).
- **Visual Studio 2022** (Community is fine) with the
  **Desktop development with C++** workload. Make sure these components are
  selected: *MSVC v143 build tools (x64/x86)*, *Windows 11 SDK* and
  *C++ CMake tools for Windows*.
- **LLVM/Clang 20 or newer** (the Clang bundled with Visual Studio is too old):

  ```
  winget install --id LLVM.LLVM -e
  ```

  The scripts expect it in `C:\Program Files\LLVM`. If you installed it
  elsewhere, set the `LLVM_DIR` environment variable to the install folder.

### Setup

All commands are meant to be run from the repository root in a regular
Command Prompt or PowerShell.

#### 1. Clone the repository

```
git clone <repository url> Hajinmon
cd Hajinmon
```

#### 2. Extract the game

Put your ISO anywhere (the repository root is fine: `*.iso` is ignored by git)
and extract it into the `game` folder:

```
py tools\xdvdfs_extract.py "Dead or Alive 4.iso" game
```

This takes a couple of minutes and produces about 6.7 GB of files. When it is
done you should have `game\default.xex`, the `.afs` archives, `game\movie` and
`game\SOUND`.

The game can also run straight from the ISO, without the extracted files:
select the `.iso` in the launcher's *Game* tab (or pass it to
`--game_data_root`). The build, however, needs the extracted
`game\default.xex`: the recompiled code is generated from it.

#### 3. Build the SDK

```
tools\setup_sdk.cmd
```

This clones ReXGlue v0.10.0 into `external\rexglue-sdk`, applies the fixes in
`patches\rexglue-sdk-v0.10.0.patch`, and builds and installs it. The first
build takes a while (30-60 minutes depending on the machine). It only needs to
be done once.

#### 4. Get the native renderer's dependencies

```
tools\setup_plume.cmd
```

This clones [Plume](https://github.com/renderbag/plume) (the graphics library
of the native renderer) into `external\plume` and the
[DXC binaries](https://github.com/renderbag/dxc-bin) (the shader compiler) into
`external\dxc-bin`, at pinned commits (`tools/setup_plume.sh` on Linux). To
build without the native renderer,
configure the game with `-DHAJINMON_PLUME_RENDERER=OFF` instead.

#### 5. Build the game

```
tools\build_doa4.cmd
```

The first build generates the recompiled C++ code into `doa4\generated\default`
from `game\default.xex` (about 20 seconds, 170 MB of sources) and then compiles
it. This produces `doa4\out\build\win-amd64-relwithdebinfo\HajinmonGame.exe`. It uses 6
parallel jobs by default; if the build runs out of memory, lower it with
`set JOBS=4` before running the script.

#### 6. Build the launcher

The launcher needs the [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0)
(`winget install --id Microsoft.DotNet.SDK.9 -e`):

```
tools\build_launcher.cmd
```

This produces `build\launcher\Hajinmon.exe`, which starts the `HajinmonGame.exe` built
in the previous step.

#### 7. Play

Start `build\launcher\Hajinmon.exe`, adjust the settings if you like and press
**Play** (see [Launcher](#launcher)). Alternatively, start the game directly
with the developer script, which also writes a debug log to `build\run.log`:

```
tools\run_doa4.cmd
```

#### 8. Make a release

```
tools\make_release.cmd
```

This builds the game and produces `build\release\Hajinmon-<version>-x86_64.exe`: a
self-contained launcher (no .NET installation needed) with the game runtime
embedded (`HajinmonGame.exe`, the ReXGlue DLLs and the Visual C++ runtime). The script
refuses to package game data. Pass `-SkipGameBuild` to reuse the current game
build.

## Launcher

The launcher starts the game (`HajinmonGame.exe`) with the command line options matching its
settings (shown in the *Advanced* tab). Settings are stored in
`%LOCALAPPDATA%\Hajinmon\settings.json`.

- **Graphics**: the renderer (Xenia, the emulated Xbox 360 GPU, or the
  experimental native Plume renderer), windowed or fullscreen, window size,
  internal resolution (1x-4x the Xbox 360's 720p), texture filtering
  (anisotropic up to 16x), anti-aliasing (FXAA; MSAA with Plume), scaling filter
  (bilinear, AMD CAS sharpening or AMD FSR upscaling), VSync, 16:9
  letterboxing, the graphics API (Direct3D 12 or Vulkan) and the GPU to use.
  With Plume, FXAA, CAS/FSR and the GPU choice are not available.
- **Game**: language (English, Japanese, German, French, Spanish, Italian),
  save folder, game (the `.iso` disc image or the folder extracted from it) and
  game executable. Empty paths use the defaults shown in grey: the release
  uses its embedded runtime; otherwise the launcher looks for `HajinmonGame.exe`,
  `game\default.xex` and, failing that, an `.iso` in its own folder and in the
  folders above it, so it works out of the box from `build\launcher`.
- **Controls**: controller remapping, keyboard and mouse controls and their
  bindings (see [Controls](#controls)).
- **Cheats**: see below.
- **Advanced**: extra command line options for the game.

### Cheats

| Cheat | Effect |
|---|---|
| Unlock all characters | Every character becomes selectable, including the hidden ones. |
| Unlock all costumes | Every costume of every character is unlocked. |

Cheats are applied once the profile has been loaded (after PRESS START). The
game writes the unlocks into the save the next time it saves, so **they become
permanent**: disabling the cheat later does not lock them again. By default
the launcher therefore copies the save folder (without the shader cache) into
`%LOCALAPPDATA%\Hajinmon\save_backups\<date>` before launching with cheats
enabled. To undo the unlocks, close the game and copy a backup back into the
save folder.

The cheats are also available as game options:
`--cheat_unlock_characters` and `--cheat_unlock_costumes`.

## Controls

- **Xbox controller** (360, One or Series, wired or wireless): works directly.
  Connect it before starting the game.
- **Other controllers** (PlayStation, Switch Pro, ...) are handled by SDL and
  usually work as an Xbox controller.
- **Keyboard and mouse**: enabled by default, next to any controller (untick
  *Enable keyboard and mouse* in the launcher's *Controls* tab, or pass
  `--no-mnk_mode`, to turn them off). Default keys:

  | Xbox | Keyboard |
  |---|---|
  | A / B / X / Y | `S` / `D` / `A` / `W` |
  | Start | `Enter` |
  | Back | `Tab` or `Z` |
  | Left stick and D-pad | arrow keys |
  | LT / RT | `Q` / `E` |
  | LB / RB | `1` / `3` |
  | Left / right stick press | `F` / `K` |

  These defaults are set by `doa4\src\input_defaults.cpp`; bindings given on
  the command line or in the config file take precedence.

### Remapping

In the launcher's *Controls* tab:

- **Controller**: choose, for every Xbox 360 button, which button of your
  controller acts as it (or *(none)*). The remapping applies to every
  connected controller. Command line: `--padbind_<button>=<controls>`, e.g.
  `--padbind_a=B --padbind_b=A,RT`. Controls: `A B X Y LB RB LT RT Back Start
  LS RS DpadUp DpadDown DpadLeft DpadRight` or `None`.
- **Keyboard and mouse**: click a binding and press a key or mouse button
  (combinations with Shift, Ctrl and Alt work too); right-click it to add an
  alternative key, clear it or reset it. *Mouse moves the right stick* turns
  the mouse into the right stick. Command line: `--keybind_<button>=<keys>`,
  e.g. `--keybind_a=S,LMB` (`None` = unbound), plus `--mnk_mouse` and
  `--mnk_sensitivity=N`.

## Configuration

The launcher covers the common settings. Without it, options are passed on
the command line (see `tools\run_doa4.cmd`); boolean options are enabled with
`--name` and disabled with `--no-name`. Useful ones:

| Option | Description |
|---|---|
| `--resolution_scale=N` | Internal resolution multiplier (1 = 1280x720, 2 = 2560x1440, 3 = 3840x2160). |
| `--window_width` / `--window_height` | Window size. |
| `--fullscreen` / `--no-fullscreen` | Start in fullscreen or in a window. |
| `--no-vsync` | Disable vertical sync (the game logic is tied to 60 Hz: not recommended). |
| `--anisotropic_override=N` | Texture filtering: `-1` game default, `0` off, `1`-`5` = 1x/2x/4x/8x/16x. |
| `--swap_post_effect=fxaa` | Anti-aliasing: `none`, `fxaa` or `fxaa_extreme`. |
| `--present_effect=fsr` | Scaling filter: `bilinear`, `cas` or `fsr`. |
| `--no-present_letterbox` | Stretch the image instead of keeping 16:9. |
| `--gpu_backend=vulkan` | Graphics API: `d3d12` (default) or `vulkan`. |
| `--d3d12_adapter=N` / `--vulkan_device=N` | GPU to use with Direct3D 12 / Vulkan; `-1` picks the dedicated GPU. |
| `--user_language=N` | Game language: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian. |
| `--user_data_root=PATH` | Folder for saves and the shader cache (default `Documents\Hajinmon`). |
| `--game_data_root=PATH` | Folder with the extracted game files, or the `.iso` disc image itself. |
| `--alloc_top_down` | **Required**: allocates the game's memory like the Xbox 360 does. Without it some characters' animation scripts break (e.g. Hayabusa is distorted) and the game can crash in character select. |
| `--render_target_path_d3d12=rov` | More accurate but much slower blending path. |

In game, `` ` `` (backtick) opens the runtime console and `F4` the settings.

## Native renderer

`--renderer=plume` replaces the Xbox 360 GPU emulation with an experimental
native renderer built on [Plume](https://github.com/renderbag/plume). The
game's shader microcode is translated to HLSL and compiled with DXC on
background threads; the compiled shaders are cached in the user data folder
(`cache\plume`), and the shaders seen in earlier sessions are compiled again at
startup, so the first time a new shader appears is the only stall. It uses
Direct3D 12 on Windows and Vulkan on Linux, where it presents to an X11 window
(XWayland on Wayland desktops; the AppImage sets `SDL_VIDEO_DRIVER=x11` with
`--renderer=plume`). Vulkan has been tested on Linux with a software driver
only.

| Option | Description |
|---|---|
| `--renderer=plume` | Use the native renderer (`xenia`, the default, is the emulated GPU). |
| `--plume_backend=d3d12` | Graphics API: `auto`, `d3d12` or `vulkan` (Windows only; Linux always uses Vulkan). |
| `--plume_resolution_scale=N` | Internal resolution multiplier, 1-4 (default 2). |
| `--plume_msaa=N` | Samples per pixel (2, 4 or 8, default 4) of the surfaces the game renders with MSAA. |
| `--plume_anisotropic=N` | Texture filtering: `-1` game default, `0` off, `2`-`5` = 2x/4x/8x/16x. |
| `--no-plume_vsync` | Present without waiting for the vertical blank (tearing). |
| `--no-plume_letterbox` | Stretch the image instead of keeping 16:9. |
| `--plume_async_shaders` | Skip draws whose shaders are still compiling instead of waiting for them. |

`F3` saves a screenshot to `Pictures\Hajinmon`. For debugging,
`--plume_capture_path=FOLDER` saves a frame every `--plume_capture_interval`
frames and `--plume_dump_shaders=FOLDER` writes the HLSL of every shader.

## Troubleshooting

- **Low frame rate**: check the `DXGI adapter` line at the top of
  `build\run.log`; it should name your dedicated GPU. Lower
  `--resolution_scale` if needed.
- **Graphical glitches after updating the SDK patch**: delete
  `Documents\Hajinmon\cache\shaders` to rebuild the shader cache.
- **Crashes**: `build\run.log` contains the call stack of the failing game
  function (lines starting with `doa4 stack`). Recompiled functions are named
  `sub_XXXXXXXX` after their address in the original executable.
- **Frame captures**: while the game is running, create
  `build\shots\capture.req` (it may contain a number of frames to capture in a
  burst, e.g. `echo 30 > build\shots\capture.req`). Convert the captures with
  `py tools\raw2png.py build\shots\capture_0.raw`.
- **Hangs**: set `DOA4_STACK_DUMP_SECONDS=N` (and optionally
  `DOA4_STACK_DUMP_COUNT=K`) before starting the game to log the stacks of all
  threads every N seconds.

## Repository layout

| Path | Contents |
|---|---|
| `doa4/` | The ReXGlue project: `doa4_manifest.toml` (recompilation settings), `src/` (application code, game fixes and cheats), `generated/` (SDK build glue; the recompiled C++ code is generated into `generated/default/` by the build and is not committed), `config/` (extra function definitions). |
| `launcher/` | The launcher (C# / Windows Forms, .NET 9). |
| `renderer/` | The native renderer: Xenos microcode to HLSL translator, shader cache, render targets and resolves, textures and pipelines on Plume, and `hajinmon_shader_tool`, which checks offline that every shader of the executable translates and compiles. |
| `patches/` | Fixes to the ReXGlue SDK runtime, applied by `tools\setup_sdk.cmd`. |
| `tools/` | Build/run scripts and analysis tools. |

### Analysis tools

| Tool | Purpose |
|---|---|
| `xdvdfs_extract.py` | Extracts Xbox / Xbox 360 disc images. |
| `xex_info.py` | Prints the XEX header (entry point, imports, SDK version...). |
| `xex_decrypt.py` | Decrypts the XEX into a memory image (`py -m pip install pycryptodome`). Example: `py tools\xex_decrypt.py game\default.xex build\default.bin`. |
| `xex_imports.py` | Lists the functions in `.pdata` and the kernel imports. |
| `ppc_dis.py` | Minimal PowerPC disassembler for quick checks. |
| `find_setjmp.py` | Looks for setjmp/longjmp candidates. |
| `find_missing_funcs.py` | Finds functions only reachable through data pointers. |
| `raw2png.py` | Converts frame captures to PNG. |

## How it works

1. `rexglue codegen` reads `game\default.xex` and translates every function
   into C++ in `doa4\generated`. The build runs it automatically when the
   manifest or its inputs change; it can also be run by hand:

   ```
   external\rexglue-sdk\out\install\win-amd64\bin\rexglue.exe codegen doa4\doa4_manifest.toml
   ```

2. The manifest fills in what the analysis cannot find on its own: the
   setjmp/longjmp addresses, functions only reached through tail calls or data
   pointers, the XAPI fiber functions (replaced with the runtime's native
   fibers) and mid-asm hooks for game fixes (`doa4\src\game_fixes.cpp`).
3. The runtime (kernel, Xenos GPU emulation on Direct3D 12 or Vulkan, XMA audio,
   input)
   comes from the ReXGlue SDK. The patch in `patches/` fixes the issues found
   while bringing up DOA4:
   - host threads did not initialize the floating-point state, which unmasked
     FP exceptions and crashed the audio mixer when the intro video started;
   - `D1MODE_V_COUNTER` (the current scanline) was a constant, which can stall
     games that synchronize with the beam position;
   - the GPU read pointer is now written back packet by packet;
   - MIN/MAX blending now applies the blend factors like Xenos does, on both
     renderers (fixes the white box behind the PRESS START text);
   - Vulkan: the texture exponent bias was read from the wrong fetch constant
     word, which scaled most textures to black, and new textures used an
     invalid barrier; `--gpu_backend` selects the renderer;
   - the default GPU is the dedicated one on hybrid-graphics laptops, on both
     renderers;
   - `--alloc_top_down` allocates 4 KB-page virtual memory top-down;
   - `--user_language` also sets the language returned by `XGetLanguage`, so
     the game follows it;
   - `--padbind_*` options remap the buttons of physical controllers;
   - `--game_data_root` also accepts a disc image (`.iso`), read in place;
   - `--log_guest_fps` logs the guest frame rate.

4. The native renderer (`renderer/`) keeps the SDK's GPU front end (registers,
   command stream, interrupts, shader microcode analysis) and replaces the
   Xenos emulation behind it. Shaders go through an HLSL backend of the SDK's
   shader translator; the guest's physical memory is mirrored on the GPU for
   vertex fetches; each EDRAM surface becomes a host render target (MSAA
   included), and resolves copy them to host textures used directly when the
   game samples or presents them, instead of going through guest memory;
   guest textures are untiled on the CPU into bindless descriptor arrays.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the recompilation
  toolkit and runtime this port is built on.
- [Xenia](https://xenia.jp/), whose kernel and GPU emulation ReXGlue builds on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), which pioneered
  Xbox 360 static recompilation.
- [Plume](https://github.com/renderbag/plume), the graphics library of the
  native renderer, and [DXC](https://github.com/microsoft/DirectXShaderCompiler),
  its shader compiler.

## License

Hajinmon is released under the [BSD 3-Clause License](LICENSE). The SDK patch
in `patches/` falls under the ReXGlue SDK's own BSD 3-Clause license. Releases
also include `THIRD_PARTY_NOTICES.txt` with the licenses of the bundled
libraries (generated by `tools/third_party_notices.py`).

*Dead or Alive* is a trademark of Koei Tecmo. This project is not affiliated
with or endorsed by Koei Tecmo or Microsoft, and it contains no game data.
