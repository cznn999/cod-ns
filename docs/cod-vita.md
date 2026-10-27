# cod-vita

A static recompilation of **Call of Duty** (Infinity Ward, 2003) — the single-player
game — from its x86 Windows binaries to portable C. It runs natively on **macOS**
and on the **PlayStation Vita**.

No game data is included. You need your own copy of the game: the build recompiles
*your* `CoDSP.exe` and game DLLs, and the game reads its data from your
installation.

## Status

- macOS: the whole single-player game: intro videos, menus, missions, saves,
  music, voices and 3D sound.
- PS Vita (Vita3K, 333 MHz): boots, plays the intro videos and the menus, loads
  and plays the missions; around 40 fps in the training level. Performance work and
  real-hardware testing are in progress.

## How it works

1. `tools/recomp` disassembles the engine executable `CoDSP.exe` and the three
   game DLLs it loads at run time (`gamex86.dll`, `cgamex86.dll`, `uix86.dll`) with
   Capstone, finds their functions and translates every one of them to C. Each DLL
   is rebased (through its relocations) into one flat 32-bit guest address space,
   next to the executable; the generated code runs against that guest memory with
   an exact x87 model where the game needs it.
2. `runtime/` maps the modules (applying the relocations and wiring the imports at
   load, as `LoadLibrary` does), runs the generated code and holds the port's fixes
   (`runtime/src/cod_fixes.c`).
3. `shim/` reimplements the parts of Windows the game uses: kernel32/user32/gdi32,
   winmm, the `opengl32.dll` the renderer loads (every `gl*` entry point generated
   from the GL headers by `tools/gen_gl.py`, `wgl*`, OpenGL on the desktop and
   vitaGL on the Vita), the **Miles Sound System 6.5** API (a software mixer on
   SDL: 2D and 3D samples, WAV and MP3 streams through dr_mp3) and input
   (keyboard/mouse on the desktop, the controller and touch panels on the Vita).

## The executable

The single-player executable of the retail CD (1.0) and of the official patches
is protected by SafeDisc: its code is encrypted on disk, so it cannot be
recompiled, and this project does not remove the protection. The build needs an
unprotected `CoDSP.exe` of version 1.3 (the last single-player update, timestamp
`0x40443FDA`) next to the 1.5 data, such as the one of a digital release. The game
DLLs and the data of the CD and the official 1.5 patch are used as they are.

## Building

Requirements: CMake 3.20+, Ninja, Python 3 with `capstone` (and `Pillow` for the
Vita build), and your game installation, whose directory (the one with `CoDSP.exe`
and `main/`) is expected at `gamedata/pc` (or pass `-DCOD_DIR=/path/to/the/game`).

### macOS

```sh
brew install cmake ninja sdl2
pip3 install capstone
cmake -S . -B build -G Ninja
cmake --build build
build/cod1 gamedata/pc
```

Click in the window to capture the mouse; Cmd releases it. The game's console
output goes to `cod1.log` (`[con]` lines).

### PlayStation Vita

Needs [VitaSDK](https://vitasdk.org). The private vitaGL build (pinned commit plus
the patches in `tools/vitagl/`) is recommended:

```sh
tools/build_vitagl_vita.sh
cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita
```

Install `build-vita/cod1.vpk` and copy the game directory to `ux0:data/cod1/pc`.
The LiveArea pictures are made from the game's own art at configure time, so they
are not part of this repository either.

The game's command line goes in `ux0:data/cod1/args.txt` (default
`+set com_hunkMegs 96 +set r_flares 0`). Controls:

| Vita | Game |
| --- | --- |
| left stick | move |
| right stick / front touch | look / aim (menus: cursor) |
| R | fire (menus: click) |
| L | aim down the sights |
| ✕ | stand up / jump (menus: select) |
| ○ | crouch (menus: back) |
| □ | reload |
| △ | use |
| D-pad up / down | grenade / prone |
| D-pad left / right | lean |
| SELECT | objectives |
| START | menu |
| rear touch, upper / lower half | next weapon / melee |

## Tools

| Tool | Purpose |
| --- | --- |
| `tools/recomp/` | The recompiler (`gen.py`, `analyze.py`, `pe.py`) |
| `tools/gen_gl.py` | Generates `shim/src/opengl32_gen.c` from the OpenGL headers |
| `tools/qa_vita3k.sh` | Runs the Vita build in Vita3K with a scripted input and screenshots |
| `tools/make_vita_livearea.py` | LiveArea pictures from the game's art |

On the Vita the renderer's back end (the CPU side of every draw) runs on a second
core while the next frame's game logic runs (`shim/src/render_async.c`;
`COD_ASYNC_RENDER=0` turns it off), and the generated code keeps x87 values in
single precision (`-DCOD_X87_FLOAT=OFF` for exact double precision).

Diagnostics (environment variables; on the Vita, `ux0:data/cod1/env.txt`):
`COD_INPUT` scripted input and screenshots, `COD_LOG_SYNC` unbuffered log,
`COD_MSS_DEBUG` Miles calls, `COD_DRAW_STATS` per-frame draw totals, `COD_PEEK`
guest memory at probes (`-DCOD_PROBES=...`), `COD_PROFILE=ON` sampling profiler.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Activision
or Infinity Ward. It contains no game code or assets: the recompiled code is
generated locally from the user's own copy.

The code in this repository is under the MIT license (see `LICENSE`), except the
patches in `tools/vitagl/`, which modify vitaGL and follow its license (LGPL-3.0),
and `third_party/dr_libs/dr_mp3.h` (public domain / MIT-0). The Vita build links
vitaGL (LGPL).
