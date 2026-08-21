# psp-bake

`psp-bake` converts legally obtained Jazz Jackrabbit 2 game files into the
cache format used by the PSP port of Jazz2 Resurrection. The distributed tool
contains the engine-owned metadata and UI assets, but no original game data.

The converter runs on 64-bit Windows and Linux. It uses the same libxmp-based
music decoder and IMA ADPCM encoder as the PSP, so host-baked and on-device
caches use the same runtime format.

Jazz Jackrabbit 2 is copyright Epic MegaGames, Inc. (now Epic Games, Inc.). This
tool is part of an unofficial fan project and ships none of their data. It reuses
the compatibility converters from
[Jazz2 Resurrection](https://github.com/deathkiller/jazz2-native) by
Dan R. ([@deathkiller](https://github.com/deathkiller)). The PSP port and this
baker are by jakstern, 2026. Licensed under the GNU GPL v3, see
[LICENSE](../../LICENSE).

## Using a packaged build

Open a terminal (PowerShell on Windows) and run:

```text
psp-bake --source "path/to/Jazz Jackrabbit 2/Source" --output "path/to/PSP/GAME/JAZZ2ENG"
```

`--source` can point either to the game's `Source` directory or to its parent.
The output directory receives a new `Cache` child. Copy that `Cache` directory
next to the game's `EBOOT.PBP`; if the output already is the EBOOT directory,
no additional copy is needed.

Alongside the converted assets, `Cache/Levels.idx` stores the canonical filename
and display name of every selectable level. The PSP menus read this small catalog
directly instead of opening every converted level during startup.

Run `psp-bake --help` for the complete interface. Useful modes are:

```text
# Validate paths and the packaged Content data without writing anything
psp-bake --source <game> --output <destination> --validate-only

# Explicitly discard an old cache and rebuild it
psp-bake --source <game> --output <destination> --clean

# Resume or regenerate only missing/incomplete music streams
psp-bake --source <game> --output <destination> --music-only
```

Paths containing spaces and non-ASCII characters are supported on both
platforms. Existing complete music streams are detected and retained. Music in
J2B, IT, XM, S3M, and MOD formats is supported; MO3 is intentionally unsupported.

Exit status `0` means success, `1` means conversion or output failure, and `2`
means invalid command-line arguments or inputs. Errors are written to standard
error, making the tool suitable for release scripts as well as interactive use.

## Building with the Makefile

`Tools/psp-bake/Makefile` produces both binaries into `Tools/psp-bake/build`:

```sh
make -C Tools/psp-bake            # both
make -C Tools/psp-bake linux      # -> build/psp-bake
make -C Tools/psp-bake windows    # -> build/psp-bake.exe
make -C Tools/psp-bake clean
```

The Linux build is native and needs CMake, a C++17 compiler, and zlib/libxmp
development files. The Windows build needs only Docker: it cross-compiles with
MinGW-w64 in an image built from `Dockerfile.mingw`, which carries statically
linked zlib and libxmp. Together with the static libgcc/libstdc++/winpthread in
`cmake/mingw-w64-x86_64.cmake`, `psp-bake.exe` imports nothing but Windows
system DLLs and can be shipped on its own.

## Building on Linux by hand

Install CMake, a C++17 compiler, zlib development files, and libxmp development
files. Package names include `zlib-devel libxmp-devel` on Fedora and
`zlib1g-dev libxmp-dev` on Debian/Ubuntu.

```sh
cmake -S Tools/psp-bake -B build/psp-bake -DCMAKE_BUILD_TYPE=Release
cmake --build build/psp-bake --parallel
cmake --install build/psp-bake --prefix build/psp-bake-install
```

Create a redistributable `.tar.gz` containing the executable, documentation,
and required Content assets with:

```sh
cmake --build build/psp-bake --target package
```

The target machine must have compatible zlib and libxmp runtime libraries. A
distribution pipeline may link those libraries statically or package them
according to the target distribution's rules.

## Building on Windows by hand

Install Visual Studio with C++ support, CMake, and vcpkg, then install the two
dependencies for the selected architecture:

```powershell
vcpkg install zlib:x64-windows libxmp:x64-windows
cmake -S Tools/psp-bake -B build/psp-bake `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/psp-bake --config Release --parallel
cmake --build build/psp-bake --config Release --target package
```

The generated ZIP includes the executable, Content data, and detected non-system
runtime DLLs. If CMake cannot locate a dependency DLL while packaging, pass its
directory through `-DPSP_BAKE_RUNTIME_DIRS=C:/path/to/bin`. A static vcpkg
triplet avoids runtime DLLs. For a manually supplied static libxmp, also
configure with `-DPSP_BAKE_STATIC_LIBXMP=ON`.

## Developer notes

For the normal PSP project workflow, `make REBAKE=1` in the repository root
builds and runs the tool, stages assets under `Build/Assets/Cache`, and
`make deploy` ships them with the EBOOT. That workflow uses its own native
build tree in `Tools/psp-bake/build`, separate from the per-platform trees this
directory's Makefile creates. An EBOOT-only installation can instead put
original game files in `Source`; missing assets are generated on the PSP itself.

Current output consists of:

- converted episode and level data, plus slim `Tilesets/*.j2tpsp` palette/collision sidecars;
- `texture.pak` with PSP-native tilesets, sprites, masks, and frame metadata;
- `audio.pak` with sound effects;
- `Music/*.wav` with streamable 22.05 kHz stereo IMA ADPCM (expanded to the PSP's 44.1 kHz output rate during playback);
- cache descriptors and the Sony savedata `ICON0.PNG`.

`PspGpuFormat.h` is shared by the baker and PSP loader. Format changes must be
made in both consumers and accompanied by a pack-version increment. Keep the
incremental pack writer usable on memory-constrained PSP hardware; the complete
texture pack must never be accumulated in RAM.
