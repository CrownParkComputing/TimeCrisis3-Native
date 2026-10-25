# Validation

## Public CI

The GitHub workflow runs nine component tests on Linux, using synthetic fixtures and no game assets. This covers inventory parsing, reference-speed replay acceptance, rendering capability gates and option bounds, USB detection/hotplug and border geometry, local two-process packet delivery, checked original receive-queue contracts, and shared pause/quit/settings. Run the commands in the main README locally. Unix socket creation must be permitted.

The replay acceptance tests explicitly reject 40 FPS progression even with 120 presented frames, a freeze hidden by a good average, mismatched initial states/inputs and a run that ends before required stage milestones. These tests validate the checker; they do not prove that the game has passed it.

## Headless Nuklear navigation

After exporting/building the pinned source, test the real game menu adapter:

```sh
c++ -std=c++20 -O2 -Ihost -Ithird_party/nuklear -I.local/pcsx2-source/3rdparty/imgui/include tests/game_menu_test.cpp host/game_menu.cpp .local/core-build/3rdparty/imgui/libimgui.a -lSDL3 -lfreetype -o .local/game-menu-test
.local/game-menu-test
```

This checks keyboard navigation, actual rendering actions, hardware capability gates, Sinden controls, cabinet action/resume dispatch, the TC4 twin-launch gate and draw-index rollover. The source uses the pinned ImGui configuration; match its compiler flags if your dependency build customizes them.

## Live integration

These development scripts need an active Linux desktop, private inputs and the built executable. They currently use Hyprland, XWayland, `xdotool` and, for pixel comparisons, `grim` and Pillow. Run only against a fresh diagnostic session; the scripts send real input.

- `tools/test_rendering_ui.py --pid PID`: single-cabinet applied settings/persistence and paused redraw.
- `tools/test_cabinet_ui.py [--quit]`: shared pause, both renderers, persistence and optional normal quit.
- `tools/test_paused_preview.py`: actual pixels, reversible FXAA/CAS/sharpness and exactly two guest fields per resolution preview; restores rendering preferences.
- `tools/test_lightgun_menu.py`: live per-view border pixels and geometry, matching original aim rectangle, unchanged paused guest fields, shared persistence/restoration and original cabinet test switch/resume.
- `tools/sample_guest_progress.py --seconds 60 --camera`: read-only guest progress and optional diagnostic camera observations; output is not a release replay.

The live checks above were exercised during development. They do not establish full-stage correctness. Synthetic border detection/mapping tests need a physical-gun follow-up for tracking and hit accuracy.

## Gameplay release gate

A valid comparison needs matching private game, initial-state and input identities, completed stage milestones beyond the reported stage-one freeze, guest field progression near reference speed, bounded stalls, camera observations and matching audio sample progression. See [REPLAY_CONTRACT.md](../tests/REPLAY_CONTRACT.md). The legacy stage-one savestate omits arcade I/O state and is rejected by the launcher.

Previous short runs observed approximately 59.94 guest fields/s, including a one-minute sample near 59.8–59.9 fields/s per cabinet. That sample did not include complete-stage milestones, so it cannot close the freeze or 60 FPS acceptance gate. Movies may produce 29.97 unique frames while progressing at full board speed.

## Optional native scalar proof

The optional `TC3_BUILD_NATIVE_BLOCK_TESTS=ON` target requires the exported core and its development build. It compares a seven-instruction native block against the unchanged pinned opcode implementation over 10,000 states. It is not enabled in the playable baseline and does not establish whole-game native coverage.
