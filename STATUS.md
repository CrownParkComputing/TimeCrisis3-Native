# Reconstruction status

The Linux executable runs TC3 with an in-process PCSX2-derived compatibility
layer, Vulkan, SPU2 audio and a DriveClub-style Nuklear menu. The old ground-up
project and calibrated reference remain intact. Whole-game native translation
and the former stage-one freeze fix are not claimed complete.

## Linked arcade screens

The default launcher now starts two isolated cabinet workers in adjacent windows.
The original arcade menu reports **LINK: ON 2**. A solo LINK PLAY run reached
stage one with different left/right cameras; the right showed the active player
with a 1P marker while retaining zero credits. No artificial second-player join,
mirrored frame, or fabricated game reply is used.

The native transport preserves the original ILSOCK packet format, descriptors,
receive rings, ILDRV workers and SIF replies. Its adapter is restricted to checked
module exports and receive-loop instructions. Empty receives yield through the
original guest DelayThread and resume the original receive loop. Host socket
operations never block EE/IOP execution. Loader-owned header links are excluded
from runtime code validation; regression tests cover their observed mutation.

Each worker has independent SRAM/cards, a separate GS mapping name and its own
Vulkan output. Opening either Nuklear menu pauses both guests. Rendering changes
publish one atomic settings transaction to both workers; closing the menu resumes
both. Quit closes the pair normally; closing or rebooting one side affects the
other. Shared pause ownership avoids resuming the other worker while its menu
remains open. One worker is muted so the pair behaves as a single machine with
one sound source.

Current presentation uses **two adjacent borderless windows** (one fullscreen
view per display, or tiled halves on a single monitor); a single combined window
is still outstanding. Complete-stage synchronization, disconnect recovery,
paired savestates and audio mixing also require further validation. One-sided
F1/F3 save/load is disabled for linked workers. The historical
stage-one freeze point has not been cleared by this work.

## Cabinet service options (direct EEPROM)

The original cabinet options no longer require the F9 test menu. F10 →
Cabinet / Service edits player's life, difficulty, hit color, free play, game
cost, continue cost and attract sound directly. **Save & Reset Machine** writes
the `TST-03A` EEPROM block with a valid additive checksum (verified against
captured cabinet SRAM), mirrors it into the backup region, publishes it to the
peer worker through the shared control state and resets both machines together.
Live run verified: setting free play produced `0x10=1` with a valid checksum in
both cabinets' `sram.bin`, both workers logged `synchronized cabinet reset`,
and both attract loops displayed `FREE PLAY`. The attract-sound field offset is
inferred from cabinet SRAM captures (both images held `0x024=1`); it is not yet
confirmed against the game's own OTHERS page.

## Rendering and menu

The menu is consolidated into File, Display & Rendering, and Cabinet / Service.
Working controls: window mode, twin launch/default, display size, aspect ratio,
internal resolution 1–4x, frame skip, VSync and Sinden border mode/width. The
FXAA/CAS/sharpness controls were removed from the visible page. Device support
gates CAS internally. DLSS presets are exposed only when a capable GPU is
detected and are stored settings — an actual DLSS backend is not implemented.
Rendering settings update the real GS/Vulkan state, are saved, and propagate to
the second cabinet. Game/video rates and speed are visible live; core OSD is
disabled.

DLSS execution, temporal FSR, RCAS, jitter, motion vectors and frame generation
remain unimplemented. The pinned source is preserved;
the playable CMake hook generates product-specific copies for patch-loader
suppression, process-isolated GS allocation, and the IOP transport boundary.
The external GameDB/PNACH/cheat loader is bypassed.

## Boot presentation

A per-game splash renders while the game boots, reporting the live stage —
VM/Vulkan start, security dongle and IOP kernel, DVD media and module load,
then JVS/ILSOCK link initialization — with a progress bar. It clears as soon as
the guest produces real frames (`game_fps >= 0.5`), and re-arms on the
synchronized service reset. On a linked pair the standby screen covers the
secondary display while the original test menu is open.

Paused menu rendering now uses MTGS idle presents. The earlier headless menu
test missed the live pause lifecycle; live integration checks cover it now.
Paused presentation now also reapplies FXAA/CAS/sharpness to a clean retained
GPU image; it previously re-presented the old processed image and omitted CAS.
Resolution changes advance two real video fields per cabinet to redraw at the
new internal resolution, then auto-pause with the menu open. Other effect edits
do not advance gameplay. Preview resources are released before device teardown.

## Validation

* Nine public component CTest cases pass: inventory, replay progress, temporal
  capability gates, rendering bounds/packed control, Sinden detection/geometry,
  two-process packet transport, original receive queues/module guards and shared
  pause/quit/settings. The additional optional scalar semantics proof also passed
  during development.
* The headless Nuklear check passes keyboard actions, device capability gates,
  geometry indices and 64K vertex-offset rollover.
* `.local/rendering-ui-validation.json` records six real GS settings toggled and
  restored through the single-cabinet menu.
* `.local/cabinet-ui-validation.json` records the six settings applied/persisted
  on both renderers, paused guest counters with advancing menu presents,
  synchronized resume, and normal Quit on both workers.
* `.local/link-analysis/evidence.json` retains original firmware hashes,
  recovered contracts, packet traces and solo spectator-camera observations.
  `.local/cabinet-{left,right}-solo.png` captures the distinct solo views.
* `.local/paused-preview/validation.json` records actual pixel comparisons:
  FXAA/CAS/sharpness change the paused right view; undoing effects recovers the
  exact original pixels; only resolution edits advance guest fields (two each).
* `.local/preview-stage1-progress.json` contains a 60-second real-process capture
  at 100 ms intervals: left approximately 59.82, right 59.92 guest fields/s.
  Camera observations and a normal-input combat smoke were captured, but no
  completed section milestones or deterministic replay identities were supplied.
  It deliberately does not claim the reported freeze point or a release pass.

Observed initial gameplay runs near 59.94 FPS/100% at native resolution. Movies
normally render 29.97 frames/s at 100% speed. Full-stage frame-time distributions,
camera/aim/audio equivalence and the old freeze point still need matched replays.
Neither a 60 Hz menu nor generated frames can satisfy the guest-speed gate.

## Native vs emulated split

"Native" means code owned by this project running directly on Linux; "emulated"
means original PS2/System 246 code executing inside the in-process
PCSX2-derived compatibility core.

| Layer | Split |
| --- | --- |
| Host, UI, input & cabinet layer (SDL3, Nuklear, EEPROM writer, `control.bin` sync, Unix link transport) | ~100% native |
| Arcade I/O & link hardware (NA-JV JVS, gun scaling, SRAM, ILSOCK/ILDRV transport) | ~100% native HLE |
| GPU presentation (GS → Vulkan, scaling, widescreen, pacing) | Hybrid: native Vulkan backend driven by the emulated GS stream |
| Game code & audio (original EE/VU/IOP programs, SPU2 mixing) | Emulated via x86-64 JIT; ~0% statically recompiled |

Approximate subsystem weight: **~45% native host/arcade infrastructure, ~55%
emulated core**. **0% of the game's own code is statically translated today.**

Path to 100% native: static EE/VU recompilation (PS2Recomp), native IOP module
contracts (the ILSOCK/ILDRV transport already demonstrates this), a native GS
renderer fed by the game's draw stream, a native SPU2-class mixer, then
matched-replay validation for full-stage completion, gun calibration, audio
equivalence and the stage-one freeze point.

## Native translation and Android

One seven-instruction scalar block has an isolated proof against original PCSX2
handlers over 10,000 states. It remains dormant in playable builds; EE/IOP/VU
execution still uses the compatibility layer. Expand native coverage using its
canonical state, device ordering and invalidation semantics.

Android with touchscreen shooting and pedal controls is requested, using TC1/TC2
as the interaction reference. No APK or touch controls are implemented yet.
Android NDKs exist locally, but the pinned ARM64 EE/VU/IOP recompilers are missing
or stubbed. Android execution, SDL/Vulkan lifecycle, asset storage, multitouch
and physical-device validation remain required. Device details are pending.

Old fresh-process stage-one states omit ACJV's enabled latch, so the launcher
rejects them. Paired snapshots need additional transport/session state. Preserve
normal boot until versioned arcade serialization and replay are implemented.

## Public frontend additions

Sinden Auto/On/Off detection uses physical USB sysfs descriptors; per-view white
borders inset the GS output and its original aim mapping together. Width and mode
are saved and shared between TC3 workers. Physical tracking and independent
two-gun routing remain unvalidated/unimplemented respectively.

Cabinet/Test dispatches original JVS test, direction, Button 1, service and coin
controls. Actions resume the guest and release momentary switches after six guest
fields. The game manifest gates twin launch to TC3; TC4 cannot start the TC3 pair.
See the public setup, dual-screen, Sinden, maintenance and testing guides in docs/.

Live border/maintenance regression passed: both cabinet borders update while
paused without advancing guest fields; each core uses the same inset rectangle
for presentation and aim mapping; width/mode persist and restore. Original test
DIP toggles are dispatched and execution resumes. This remains a software test,
not a physical Sinden tracking or accuracy result.
