# Call of Duty (2003) for Nintendo Switch

A native Nintendo Switch port of the single-player campaign of **Call of Duty**
(Infinity Ward, 2003), built on [cod-vita](https://github.com/HenryKun55/cod-vita)'s
static recompilation of the PC game. The game's x86 code is translated to C at
build time and compiled for the Switch's ARM CPU: no emulator runs on the console.

> [!IMPORTANT]
> **You need your own copy of the game from [Steam](https://store.steampowered.com/app/2620/).**
> This repository contains no game code or data. The build recompiles *your*
> `CoDSP.exe` and game DLLs, and the game reads its data from your installation.
> The retail CD version cannot be used: its executable is encrypted by SafeDisc,
> and this project does not remove copy protection.

## Installing
Download the newest build from **releases** <br>
Put the **cod1.nro** in **/switch/**
```
SD:/switch/
├── cod1.nro
└── cod1/
    └── pc/            <- everything from your game folder, you may delete CoDMP.exe/CoDUOMP.exe to save some space
        ├── CoDSP.exe
        ├── gamex86.dll
        ├── cgamex86.dll
        ├── uix86.dll
        └── main/
```

**Start it with title takeover**: hold **R** while launching any game from the HOME
menu, then pick `cod1.nro` in hbmenu. That gives the port the full ~3 GB of RAM.
Applet mode is not supported.

## Status

- Runs on real hardware: menus, missions, saves, music and voices.
- 720p handheld, 1080p docked (switches when you dock or undock).
- Performance work is in progress.
- United Offensive is not supported yet.

## Controls

| Switch | In game | In menus |
|---|---|---|
| Left stick | Move | Arrow keys |
| Right stick | Look / aim | Cursor |
| **ZR** | Fire | Click |
| **ZL** | Aim down the sights | |
| **R** | Next weapon | |
| **L** | Melee | |
| **A** | Stand up / jump | Confirm |
| **B** | Crouch | Back |
| **Y** | Reload | |
| **X** | Use | |
| D-pad up / down | Grenade / prone | Arrow keys |
| D-pad left / right | Lean left / right | Arrow keys |
| **−** | Objectives | |
| **+** | Menu | Back |
| Touch screen (handheld) | Drag to look | Cursor, tap to click |

The right stick's speed follows the game's own mouse sensitivity
(*Options → Controls*); `cod1.cfg` below can scale it further.

## Requirements

- A Switch that can run homebrew
- Call of Duty from **Steam**. `CoDSP.exe` must be version 1.3 without copy
  protection; `tools/check_game_files.py` checks this for you.
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with these packages:
  ```sh
  dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau switch-pkg-config switch-tools
  ```
  (in devkitPro's MSYS2 on Windows: `pacman -S ...`).
- CMake 3.20+, Ninja, and Python 3 with [Capstone](https://www.capstone-engine.org/)
  (`pip install capstone`). The recompiler runs while CMake configures.

## Building

**1. Check your game files**

```sh
python3 tools/check_game_files.py "/path/to/Call of Duty"
```

Every line should say `OK`. `PROTECTED` or `WRONG VERSION` means that executable
cannot be recompiled.

**2. Configure and build**

```sh
cmake -S . -B build-switch -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCOD_DIR="/path/to/Call of Duty"
cmake --build build-switch
```

The result is `build-switch/cod1.nro`. The first build takes a while: the
recompiler turns about 12,700 of the game's functions into C.

<details>
<summary>Windows (devkitPro MSYS2) with Python from python.org</summary>

Capstone installs easily into the Windows Python, so the recompiler can use that
one while everything else runs in MSYS2. In `cmd`:

```bat
py -m pip install capstone
py -c "import sys; print(sys.executable)"
```

Then in the MSYS2 shell, with the path printed above:

```sh
PY="$(cygpath -u 'C:\Users\you\AppData\Local\Programs\Python\Python314\python.exe')"
cmake -S . -B build-switch -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DPython3_EXECUTABLE="$PY" \
      -DCOD_DIR="/x/games/Call of Duty"
cmake --build build-switch
```

(`X:\games` is `/x/games` in MSYS2.)
</details>

Optional: put a 256×256 `platform/switch/icon.jpg` in place before configuring to
give the `.nro` an icon.

## Settings

All optional, all in `SD:/switch/cod1/`:

| File | What it does |
|---|---|
| `args.txt` | The game's command line, e.g. `+set r_flares 0 +set com_hunkMegs 128`. Without it the port uses `+set r_flares 0`. |
| `cod1.cfg` | Controls, one per line: `stick_sensitivity=1.5`, `invert_y=1`, `touch=0`. |
| `env.txt` | Port options, `NAME=value` per line (see below). |

Options for `env.txt`:

| Option | Effect |
|---|---|
| `COD_ASYNC_RENDER=1` | Prepares each frame for the GPU on a second CPU core (experimental). |
| `COD_VBO_CACHE=1` | Keeps level geometry in GPU buffers instead of sending it every frame (experimental). |
| `COD_VSYNC=0` | Vsync off. |
| `COD_LOG_SYNC=1` | Writes the log line by line; slower, but the log survives a crash. |

The port writes `SD:/switch/cod1/cod1.log`, with the frame rate and the screen
resolution every 10 seconds. Attach it to bug reports.

## How it works

The recompiler (`tools/recomp`) disassembles `CoDSP.exe` and the three game DLLs
and writes every function out as C, all of them sharing one 32-bit guest address
space. The parts of Windows the game uses (kernel32, user32, OpenGL 1.x, the Miles
Sound System) are reimplemented on top of the host. See
[docs/cod-vita.md](docs/cod-vita.md) for the details of the recompilation.

What the Switch port adds:

| File | |
|---|---|
| `shim/src/platform_switch.c` | Window and OpenGL context (SDL2 + Mesa, compatibility profile), libnx pad and touch screen, docking. |
| `shim/include/gl_switch.h`, `shim/src/gl_switch.c` | Mesa on the Switch exports no `gl*` symbols: the functions called by name go through pointers loaded from EGL. |
| `runtime/src/memory.c` | The guest space from the newlib heap (512, 256 or 192 MB). |
| `runtime/src/main.c` | `sdmc:/switch/cod1` paths; the game runs on a thread with an 8 MB stack. |
| `shim/src/kernel32.c` | `args.txt`, CD directory without `..`. |
| `CMakeLists.txt` | Switch target: SDL2 via pkg-config, switch-mesa, NACP and NRO. |
| `tools/check_game_files.py` | Checks the game files before building. |

## Credits

- [**cod-vita**](https://github.com/HenryKun55/cod-vita) by HenryKun55: the
  recompiler and the Windows, OpenGL and Miles reimplementations this port is
  built on.
- [devkitPro](https://devkitpro.org/), [libnx](https://github.com/switchbrew/libnx),
  [SDL2](https://www.libsdl.org/) and [Mesa](https://www.mesa3d.org/) for the Switch.
- [Capstone](https://www.capstone-engine.org/) (disassembler),
  [dr_libs](https://github.com/mackron/dr_libs) (MP3).

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Activision,
Infinity Ward or Nintendo. Call of Duty is a trademark of Activision Publishing,
Inc. The repository contains no game code or assets: the recompiled code is
generated locally from the user's own copy, and builds must not be distributed.

The code is under the MIT license (see [LICENSE](LICENSE)), except
`tools/vitagl/` (LGPL-3.0, patches to vitaGL) and `third_party/dr_libs/dr_mp3.h`
(public domain / MIT-0).
