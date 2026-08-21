# Jazz Jackrabbit 2 for PSP

A native PlayStation Portable port of Jazz Jackrabbit 2, based on the
[Jazz2 Resurrection](https://github.com/deathkiller/jazz2-native) engine.

It plays the single-player campaigns as Jazz, Spaz or Lori, loads Home Cooked
levels, has music and sound, and supports ad-hoc multiplayer. It runs on real
PSP hardware and draws through the console's own GU pipeline.

No game data is included. You supply your own copy of Jazz Jackrabbit 2, and
the original files get converted into a PSP-ready cache, either on a PC or on
the console itself.

Jazz Jackrabbit 2 is copyright Epic MegaGames, Inc. (now Epic Games, Inc.).
This is an unofficial fan project. It is not affiliated with or endorsed by
Epic Games, and it contains none of their game data: no art, audio, levels or
other assets from the game.

## Preview

https://github.com/user-attachments/assets/b550d463-e7d0-42eb-9d1e-6caed82427dc

## Running the game

### What you need

* A PSP with custom firmware (a homebrew-enabled console). PSP-1000, 2000 and
  3000 all work.
* Your own copy of Jazz Jackrabbit 2: the retail release, the GOG release
  (Jazz Jackrabbit 2: The Secret Files), or the free v1.23 Shareware.
* A PC, if you want to bake the assets there. This is recommended, and it is
  required on a PSP-1000. See step 3.

### 1. Download

Get the latest engine package from the [Releases page](../../releases). It
unpacks to a folder like this:

```text
JAZZ2-PSP/
    EBOOT.PBP      the engine
    Content/       engine data, required, must sit beside the EBOOT
    Source/        empty, your original game files go here
    Cache/         empty, converted PSP-ready assets end up here
    README.txt
    LICENSE.txt
```

### 2. Copy it to the Memory Stick

Put the whole `JAZZ2-PSP` folder under `PSP/GAME/`:

```text
ms0:/PSP/GAME/JAZZ2-PSP/EBOOT.PBP
ms0:/PSP/GAME/JAZZ2-PSP/Content/
ms0:/PSP/GAME/JAZZ2-PSP/Source/
ms0:/PSP/GAME/JAZZ2-PSP/Cache/
```

You can rename the folder to whatever you want. Copying `EBOOT.PBP` on its own
will not work, because `Content/` is required.

### 3. Provide the game data

There are two ways to do this.

**Option A, bake on the PSP** (PSP-2000 and 3000)

Copy every file from your Jazz Jackrabbit 2 installation into `Source/`:

```text
Source/Anims.j2a      (retail)   or   Source/AnimsSw.j2a   (shareware)
Source/Data.j2d
Source/*.j2l, *.j2t, *.j2e, *.j2b, ...
```

Launch the game. It converts `Source/` into `Cache/` on first start and shows
progress. Keep the console powered and the Memory Stick inserted. For the full
retail game this takes roughly 30 to 40 minutes.

**Option B, bake on a PC** (required on a PSP-1000, and much faster anyway)

Run the `psp-bake` tool as described below, then copy the `Cache` folder it
produces over the empty `Cache/` in the package. `Source/` can stay empty.

On a PSP-1000, baking on the device is disabled. The original 32 MB console
does not have enough memory to convert the game files, so the engine refuses up
front instead of failing part way through. Use option B there.

### 4. Play

On the PSP: Game > Memory Stick > Jazz Jackrabbit 2.

## Baking assets on a PC

`psp-bake` converts original Jazz Jackrabbit 2 files into the PSP's cache
format. It uses the same music decoder and ADPCM encoder as the console, so a
PC-baked cache and an on-device one come out the same. Windows and Linux builds
are on the [Releases page](../../releases).

```sh
psp-bake --source "path/to/Jazz Jackrabbit 2/Source" --output "path/to/JAZZ2-PSP"
```

`--source` accepts either the game's `Source` directory or its parent. The
output directory gets a `Cache` child, so if you point `--output` straight at
the folder holding `EBOOT.PBP` there is nothing left to copy afterwards.

Other useful modes:

```sh
psp-bake --source <game> --output <dest> --validate-only   # check inputs, write nothing
psp-bake --source <game> --output <dest> --clean           # discard an old cache first
psp-bake --source <game> --output <dest> --music-only      # resume or redo just the music
```

Music in J2B, IT, XM, S3M and MOD is supported. MO3 is not. Existing complete
music streams are detected and kept, so an interrupted run resumes cheaply.
Exit status is 0 on success, 1 on conversion failure, 2 on bad arguments.

More detail in [Tools/psp-bake/README.md](Tools/psp-bake/README.md).

## Building from source

### The engine

Docker is the only requirement. The PSPDEV toolchain runs inside it.

```sh
make
```

This builds `EBOOT.PBP` and assembles the complete package at
`build/psp/JAZZ2-PSP/`, ready to copy to a Memory Stick. `make clean` removes
`build/`.

### The asset baker

```sh
make -C Tools/psp-bake            # both binaries
make -C Tools/psp-bake linux      # Tools/psp-bake/build/psp-bake
make -C Tools/psp-bake windows    # Tools/psp-bake/build/psp-bake.exe
```

The Linux build is native and needs CMake, a C++17 compiler and zlib/libxmp
development headers. The Windows build needs only Docker: it cross-compiles
with MinGW-w64 and links zlib, libxmp and the C++ runtime statically, so the
resulting `.exe` has no DLLs to ship with it.

## Project status

Working: rendering, input, gameplay, HUD, PSP-native menus and pause overlay,
Home Cooked level browsing, PCM sound effects, streamed music, profile and
progression persistence, and five-slot Sony savedata save/load. Both the PC
baker and the console's first-run preparation convert original game data into
PSP-ready levels, textures, audio, music, loading art and save icons.

The PSP-1000 uses a low-memory streaming profile and renders at 30 Hz while
keeping the 60 Hz gameplay simulation. The 2000 and 3000 run the full profile.

Multiplayer runs over PSP ad-hoc WLAN. Two consoles can play together and
either one can host. A PSP can also join a game hosted in PPSSPP by setting
Options > Network to PPSSPP, but in that mode PPSSPP has to be the host.
Runtime validation of multiplayer is still ongoing, see
[Sources/Jazz2/Multiplayer/README.md](Sources/Jazz2/Multiplayer/README.md).

## Credits and license

* Jazz Jackrabbit 2 is copyright Epic MegaGames, Inc., now Epic Games, Inc.
  None of their game data is distributed here.
* The engine this port is built on is
  [Jazz2 Resurrection](https://github.com/deathkiller/jazz2-native) by
  Dan R. ([@deathkiller](https://github.com/deathkiller)).
* PSP port by jakstern, 2026.

Released under the GNU General Public License v3, see [LICENSE](LICENSE). That
covers the engine's own source and its `Content/` files only, not Epic's game
data.
