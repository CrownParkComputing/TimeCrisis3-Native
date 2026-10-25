# Time Crisis 3 Native · Linux / Vulkan

A game-specific Linux reconstruction of the **Time Crisis 3 arcade version**, with a Vulkan renderer, SPU2 audio and a DriveClub-style Nuklear menu. The playable foundation embeds a pinned PCSX2x6 core in `tc3-native`; it has no Qt frontend or external emulator process. **Whole-game native translation remains in development.**

The default launcher runs two linked cabinet workers, each with its own arcade camera and mutable storage. A solo **LINK PLAY** session displays the uncredited partner camera on the right. Presentation currently uses two adjacent windows.

| Player 1 cabinet | Uncredited partner cabinet |
| --- | --- |
| ![TC3 player one stage one view](docs/screenshots/tc3-left-solo.png) | ![TC3 original partner camera with the 1P marker](docs/screenshots/tc3-right-solo.png) |

These are captures of the running compatibility foundation. The historical stage-one freeze point, complete-stage speed, exact aim alignment and audio equivalence remain unverified.

## Start here

- [Build and private asset setup](docs/BUILD.md)
- [Dual-screen arcade setup](docs/DUAL_SCREEN.md)
- [Controls and playtesting](PLAYTEST.md)
- [Sinden detection, borders and official Linux driver](docs/SINDEN.md)
- [Original cabinet test and maintenance menu](docs/CABINET_TEST.md)
- [Testing and current acceptance gates](docs/TESTING.md)
- [Current implementation status](STATUS.md)

The separate TC4 project is [TimeCrisis4-Native](https://github.com/CrownParkComputing/TimeCrisis4-Native).

## Run

After following the build and asset instructions:

```sh
python3 tools/run_tc3.py
# Single cabinet:
python3 tools/run_tc3.py --single
```

Use **5** for coins, **Enter** to start, **mouse / left click** to aim and fire, and hold **Space** to press the pedal. The pair boots linked by default; **F10** opens Nuklear and pauses both cabinets, arrows and Enter navigate, Esc or Backspace resumes, and **F9** toggles the original cabinet test menu on both screens.

The menu has three tabs — **File**, **Display & Rendering**, **Cabinet / Service**:

- **Display & Rendering**: window mode (arcade pair / fullscreen / windowed), twin-screen launch and linked-at-boot default, display size, aspect ratio (4:3 cabinet / 16:9 widescreen / stretch), internal resolution 1–4x, NVIDIA DLSS presets when a supported GPU is detected, frame skip, VSync and Sinden border mode/width. Everything applies atomically to both cabinets and is persisted.
- **Cabinet / Service**: the original cabinet options are edited directly — player's life, difficulty, hit color, free play, game cost, continue cost and attract sound. **Save & Reset Machine** writes them straight to the cabinet EEPROM (`TST-03A` block) with a valid checksum and reboots both linked screens together, so the pair always runs identical settings. Service credit and coin insert are also here; opening the original test menu is no longer required for ordinary settings changes.

![TC3 live Nuklear rendering menu](docs/screenshots/tc3-rendering-menu.png)

The rendering capture predates the consolidated menu. DLSS presets are stored but require a capable GPU path; frame generation remains unavailable. Android and touchscreen controls are planned; there is no Android release yet.

## Native vs emulated split

"Native" here means code this project owns and runs directly on Linux; "emulated" means original PlayStation 2 / System 246 code still executed inside the in-process PCSX2-derived compatibility core.

| Layer | What it is | Split |
| --- | --- | --- |
| Host, UI, input & cabinet layer | SDL3 windows, paired-cabinet layout, Nuklear menu, direct EEPROM writer, Sinden/mouse gun, shared `control.bin` sync, Unix-socket link transport | ~100% native C++/Python |
| Arcade I/O & link hardware | Native HLE for the NA-JV JVS board, gun scaling, SRAM, and the original ILSOCK/ILDRV link packet transport between the two workers | ~100% native C++ HLE (replaces the cabinet PCB & link hardware) |
| GPU presentation | PS2 GS primitives translated to native Vulkan draw calls with 1–4x scaling, widescreen/aspect composition and frame pacing | Hybrid: native Vulkan backend fed by the emulated GS command stream |
| Game code & audio | The original TC3 EE/VU/IOP programs and SPU2 mixing run via the x86-64 JIT recompiler | ~0% statically recompiled; executed on the compatibility core |

Roughly **~45% of the runtime is native host/arcade infrastructure and ~55% is the emulated core** by subsystem weight; **0% of the game's own code has been statically translated**. The original game still runs — the project wraps it in a fully native cabinet experience.

### Next steps toward 100% native

1. Statically recompile the TC3 EE/VU program (PS2Recomp pipeline) and retire the JIT core for gameplay code.
2. Replace IOP module emulation (ILSOCK/ILDRV, ACSRAM, JVS) with the already-documented native contracts used by the link transport.
3. Reimplement the GS renderer natively against the game's draw command stream rather than generic GS emulation.
4. Replace SPU2 emulation with a native mixer fed by the game's audio engine.
5. Validate with matched replays: full-stage completion, aim/gun calibration, audio equivalence and the historical stage-one freeze point.

## Component tests

These tests use synthetic inputs and require no game, firmware, Vulkan or SDL installation:

```sh
cmake -S . -B build -DTC3_BUILD_HOST=OFF -DTC3_BUILD_NATIVE_BLOCK_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

They cover executable parsing, replay acceptance, rendering capability gates and settings, Sinden detection/border geometry, real local packet transport, original receive-queue behavior and shared cabinet control. They do not establish completed stage progression or 60 FPS gameplay.

## Source and licensing

The compatibility foundation is [PS2Homebrew-arcade/pcsx2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6), pinned in [pcsx2.lock.json](third_party/pcsx2.lock.json). The project preserves the upstream scheduler, hardware semantics and execution backend while title-specific native blocks are validated against it. The optional scalar proof covers seven instructions, not the full game.

Project code is GPL-3.0-or-later; see [COPYING](COPYING) and [third-party notices](THIRD_PARTY_NOTICES.md). Game screenshots illustrate compatibility testing; game content and trademarks belong to their respective owners. No game media, dongle cards, BIOS, savestates or Sinden driver binaries are included. Supply your own assets.
