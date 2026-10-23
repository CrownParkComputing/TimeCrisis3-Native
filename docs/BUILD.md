# Build and assets

## Linux prerequisites

The playable target currently requires x86-64 Linux, an upstream-supported CPU, a Vulkan-capable GPU/driver, Python 3.11+, Git, CMake 3.24+, Ninja, Clang and the dependencies of the pinned PCSX2x6 source. Standalone binary packaging is unfinished.

The pinned source checks SDL3 >=3.2.6, PNG >=1.6.40, Freetype >=2.10, Zstd >=1.5.5, JPEG, zlib, LZ4, WebP, plutovg >=1.1, plutosvg >=0.0.7, rapidyaml, shaderc, curl, libpcap and Linux platform libraries including DBus/X11/Wayland as enabled by its configuration. Follow the pinned upstream dependency instructions for your distribution. The Qt UI is disabled, so Qt is not required for this target. A custom dependency prefix can be passed explicitly; no sibling research project is required.

```sh
git clone https://github.com/CrownParkComputing/TimeCrisis3-Native.git
cd TimeCrisis3-Native
mkdir -p .local
git clone https://github.com/PS2Homebrew-arcade/pcsx2x6.git .local/upstream
python3 tools/prepare_core.py --checkout .local/upstream
python3 tools/build_core.py --playable
# If dependencies were installed in a private prefix instead:
# python3 tools/build_core.py --playable --prefix /absolute/path/to/dependency-prefix
```

`prepare_core.py` exports the exact locked commit with `git archive` and records provenance. It refuses to overwrite an existing export. If a shallow upstream clone lacks the commit in `third_party/pcsx2.lock.json`, fetch that commit first. Builds use `.local/pcsx2-source` and `.local/core-build`; the original upstream checkout is not modified. The host's guarded CMake integration generates specialized core source copies in the build directory.

## Private reference inputs

The current preparation tool supports the **verified NM00012 / TST3 reference set only**. It verifies hashes in `tools/prepare_playtest.py`; an arbitrary regional release or uncalibrated dongle is not automatically supported. Place your own files in this layout, or pass another directory with `--reference`:

```text
.local/reference/
  games/
    proverb.elf
    TST1.iso
    sram.bin
    NM00012.acgame
  PCSX2x6/
    bios/246C.bin
    bios/246C.nvm
    bios/246C.mec
    memcards/NM00012.ps2
    inis/PCSX2.ini
```

`NM00012.acgame` must identify `gameid=NM00012`, `platform=246`. `PCSX2.ini` is the settings file from your calibrated reference setup. The SRAM must be 32768 bytes and have the five-point calibration expected by the preparation tool (320,220 / 90,360 / 550,360 / 550,80 / 90,80). Input calibration and dump validation are prerequisites; the tool rejects mismatches with a specific error instead of silently altering the reference.

```sh
python3 tools/prepare_playtest.py --reference .local/reference
python3 tools/run_tc3.py
```

Preparation copies mutable BIOS support files, SRAM and dongle storage into `.local/playtest`, and links the immutable DVD read-only. Reruns preserve existing settings and mutable storage. The twin launcher prepares two independent worker roots from that normalized playtest directory. Keep `.local/reference` and its media in place because the DVD links point there.

Normal boot is the default. `--with-state` only copies the legacy research state; the playable launcher rejects `--stage1` because that state omits the arcade I/O enabled latch. See [PLAYTEST.md](../PLAYTEST.md).

## Separate menu shell

The root default `tc3-native-host` target is a standalone Vulkan/Nuklear smoke-test shell, not the playable game. Building it additionally requires `glslangValidator`, SDL3 and Vulkan development libraries:

```sh
cmake -S . -B build-shell -DCMAKE_BUILD_TYPE=Release
cmake --build build-shell -j2
```

For the game use the `tools/build_core.py --playable` target described above.
