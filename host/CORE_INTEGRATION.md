# Connecting the PCSX2 core to the SDL3 host

Audit date: 2026-10-23. This is an implementation plan based on the pinned local
source and the built archive, not a claim that gameplay already runs in the new
host. No emulator process or Qt frontend is required by the proposed design.
Source references beginning `pcsx2/`, `common/`, `cmake/`, or `pcsx2-gsrunner/`
are relative to `.local/pcsx2-source/`; line numbers refer to this pinned export.

## What is already available

The core archive exists at `.local/core-build/pcsx2/libpcsx2.a`. The standalone
host has an SDL3 window, a Vulkan device/swapchain, Nuklear rendering, and validated
resize/fullscreen handling. They are separate programs/components today.

The pinned source already demonstrates a frontend without Qt:
`pcsx2-gsrunner/CMakeLists.txt:1` creates an executable and links **PCSX2_FLAGS and
PCSX2**, not a manually assembled list containing only `libpcsx2.a`. Its
`Main.cpp:952` calls the VM lifecycle directly. However, that frontend is a GS
dump runner: it sets `GSDumpReplayer::SetIsDumpRunner(true)`, uses replay-specific
message pumping, and its `Host::RunOnCPUThread` deliberately fails at line 368.
It is a reference for the link contract, not a gameplay frontend to copy intact.

Both the current host and this pinned core use SDL3. The core requires SDL3
3.2.6+ in `cmake/SearchForStuff.cmake:20` and links `SDL3::SDL3` in
`pcsx2/CMakeLists.txt:1181`; there is no SDL2/SDL3 ABI conversion to solve.

## Link contract and frontend callbacks

A read-only `nm -C` audit of the built archive found **53 Host symbols referenced
but not defined inside that archive** (subtracting its definitions from its
undefined symbols). This is an archive-local observation, not an exhaustive
final executable link check: common/dependency libraries and selected archive
members affect the actual link closure.

Implement a new frontend adapter, keeping callbacks grouped by responsibility:

| Group | Required examples | Implementation rule |
| --- | --- | --- |
| Settings | `LoadSettings`, `CheckForSettingsChanges`, `CommitBaseSettingChanges`, `SetDefaultUISettings`, `RequestResetSettings` | Own a persistent settings interface; use the core settings lock and layer APIs. Do not silently erase cabinet configuration. |
| Window/presentation | `AcquireRenderWindow`, `ReleaseRenderWindow`, `BeginPresentFrame`, `RequestResizeHostDisplay`, `IsFullscreen`, `SetFullscreen` | Return a stable SDL native-window description; coordinate SDL operations on the UI thread. There must be one swapchain owner. |
| CPU dispatch | `RunOnCPUThread`, `PumpMessagesOnCPUThread`, `RequestVMShutdown`, `RequestExitApplication` | Implement a real work queue with completion signaling for blocking requests; wake a paused CPU thread. No immediate execution on arbitrary callers. |
| Lifecycle/metrics | `OnVMStarting`, `OnVMStarted`, `OnVMDestroyed`, `OnVMPaused`, `OnVMResumed`, `OnGameChanged`, `OnPerformanceMetricsUpdated` | Publish snapshots/events to Nuklear; do not mutate its context from CPU/GS callbacks. |
| Input/UI | `BeginTextInput`, `EndTextInput`, `SetMouseMode`, `SetMouseLock`, input-device notifications, clipboard, host file selector | SDL operations belong on the UI thread. Complete asynchronous file-selection callbacks, including cancellation. |
| Reporting/translation | `ReportErrorAsync`, `ReportInfoAsync`, progress callbacks, translation implementation, plural translation, locale comparison | Surface failures rather than discarding them. Identity English translation is acceptable initially. |
| Optional frontend services | achievement notifications, game-list refresh, cover/memory-card dialogs, capture/save-state notifications, no-GUI checks | Unsupported services need explicit disabled settings or a visible unsupported response; they cannot replace VM control callbacks with no-ops. |

Callback declarations are distributed across `pcsx2/Host.h`, `pcsx2/VMManager.h`,
GS/input/UI headers. `pcsx2-gsrunner/Main.cpp:164–462,1014–1033` supplies the
reference frontend definitions. Use the compiler/linker to establish the final
adapter symbol list; do not duplicate callbacks already implemented by
`pcsx2/Host.cpp`.

Keep the new executable in the core's CMake target graph so **PCSX2** carries its
transitive libraries, generated include paths, and compile definitions. A small
explicit integration CMake fragment can add the SDL frontend target to the
prepared core source build. Simply importing the archive into the current host
without those dependencies is not a complete integration. Preserve the pinned
source and record any integration patch rather than editing an installed
emulator or relying on its executable.

## CPU, GS, and UI thread lifecycle

The SDL main thread owns the window, event pump, dialogs and frontend lifetime.
A dedicated CPU thread owns the VM and drains a synchronized task/input queue.
MTGS owns the GS render thread and its Vulkan objects. SPU2/audio and other core
workers retain the upstream lifecycle.

A bounded first boot should follow this order:

1. Perform early hardware checks. Establish application/resource/data paths,
   persistent settings and any secrets layer. Apply defaults, load private user
   settings, then call `VMManager::Internal::LoadStartupSettings`.
   `pcsx2-gsrunner/Main.cpp:121–161` demonstrates the ordering, including the
   resource font needed by the existing core ImGui manager. Package required
   resource files as well as the archive; fonts/shaders/game database cannot be
   inferred from a successful static-library build.
2. Create the SDL window and publish its native handles. Before opening GS,
   release the standalone host renderer's swapchain/surface/device if that
   renderer was used for the initial menu. Keep the SDL window alive.
3. On the CPU thread, call `VMManager::Internal::CPUThreadInitialize()`, apply
   settings, and `VMManager::Initialize(VMBootParameters, Error*)`. Check the
   result and report failure. Do **not** set dump-runner mode. Use the owned
   `.acgame` manifest as `filename`, or a validated `ArcadeBootParams` path.
4. Set `VMState::Running`; call `VMManager::Execute()` while running. Dispatch
   queued requests through `Host::PumpMessagesOnCPUThread`. While paused or idle,
   drain tasks and call `VMManager::IdlePollUpdate()` without busy-spinning.
   Pause/reset/shutdown must use the VM APIs, not a custom EE-cycle loop.
5. On exit, request stopping on the CPU thread, let `Execute()` unwind, call
   `VMManager::Shutdown(false)`, then
   `VMManager::Internal::CPUThreadShutdown()`. Keep processing UI requests until
   CPU/GS shutdown completes, join the CPU thread, and only then destroy SDL.
   Otherwise a GS callback waiting for a UI operation can deadlock shutdown.

`pcsx2/VMManager.cpp:396` initializes memory, CPU providers, USB, settings/input
sources, and optional services. Its shutdown at line 454 closes sources, waits
for save-state writes, shuts down USB/MTGS, joins snapshot workers and releases
memory. VM boot opens GS and SPU2 at lines 1759–1778; normal VM shutdown closes
SPU2/GS at lines 1939–1963 and saves arcade SRAM at line 1972. Preserve that
sequence. `VMManager.h:90` specifies a 2 MiB minimum recompiler-thread stack.

`pcsx2/MTGS.h:56–91` provides `WaitForOpen`, `WaitForClose`, `RunOnGSThread`,
`ResizeDisplayWindow`, `PresentCurrentFrame` and idle presentation controls.
Follow its thread annotations; notably `PresentCurrentFrame` is requested from
the CPU thread. See the real interactive dispatch in
`pcsx2-qt/QtHost.cpp:1197–1217` for queue semantics, without linking Qt itself.

## Window handles and single presentation owner

Translate `SDL_GetWindowProperties()` into `common/WindowInfo.h:10`:

| SDL backend | SDL3 properties | Core WindowInfo |
| --- | --- | --- |
| X11 | `SDL_PROP_WINDOW_X11_DISPLAY_POINTER`, `SDL_PROP_WINDOW_X11_WINDOW_NUMBER` | `Type::X11`, Display pointer, XID converted through `uintptr_t` into `window_handle` |
| Wayland | `SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER`, `SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER` | `Type::Wayland`, `wl_display*`, `wl_surface*` as `window_handle` |

Use drawable pixels from `SDL_GetWindowSizeInPixels`, the real scale and refresh
rate. Snapshot these on the SDL thread; avoid calling SDL window APIs from GS
callbacks. Route pixel-size/fullscreen changes to MTGS resize/update operations.
Handle Wayland surface configure events rather than assuming a requested window
size was accepted.

`GSDevice::AcquireWindow` (`GS/Renderers/Common/GSDevice.cpp:386`) consumes
`Host::AcquireRenderWindow`. `GSDeviceVK::CreateDeviceAndSwapChain` at line 2538
creates the core Vulkan device and swapchain. Its `BeginPresent` at line 2352
acquires an image; `EndPresent` at line 2440 ends the render pass, transitions the
image and submits/presents.

**Recommended first integration: GS owns presentation while the VM is active.**
Do not run `Host::frame()` from this project's `host/main.cpp` concurrently with
GS on the same SDL window. The shell's Vulkan device and GS's Vulkan device are
also distinct objects: a raw image handle from one cannot simply be sampled on
the other. Use an explicit ownership handoff at boot/shutdown, then replace the
standalone presentation path with GS idle rendering when unified menus are
ready. Keeping a second surface or copying GS to the existing swapchain is a
larger interop project, not the shortest route to correct gameplay.

## Nuklear during gameplay

`GSRenderer::BeginPresentFrame` (`GSRenderer.cpp:530`) calls
`Host::BeginPresentFrame` **before** `GSDevice::BeginPresent`. That Host callback
is suitable for transferring queued UI state, not issuing draw commands into a
render pass which has not started.

The practical overlay location is `GSRenderer::EndPresentFrame:574`, before
`g_gs_device->EndPresent()`. The existing Vulkan end-present path calls
`RenderImGui()` at `GSDeviceVK.cpp:2442`; a frame is already being presented there.

Two scoped options:

- Smallest initial adapter: keep Nuklear as the UI and translate its converted
  vertices/indices/clip rectangles into the existing ImGui foreground draw list.
  This is the pattern actually used by DriveClub's local `nk_menu.cpp:1064`.
  The core already contains the ImGui renderer; this adds no Qt frontend.
- Retain the new direct Nuklear Vulkan renderer, but adapt its resource creation
  to the **GS device**, render-pass compatibility and per-frame resources.
  `GSDeviceVK.h:55,100` exposes the device/current command buffer, but they do
  not by themselves constitute a safe lifetime/synchronization contract.
  Device recreation must invalidate/rebuild the overlay's atlas, pipeline,
  descriptors and buffers. Do not call the shell's independent queue-idle
  presentation loop from the GS thread.

For either option, create a narrow recorded overlay hook in the prepared core
source. Keep Nuklear context ownership on the render thread and submit SDL input
via a queue/snapshot. A visible menu may request pause, but must not call blocking
CPU work while that CPU thread is waiting for GS.

## Audio and cabinet input

Retain the core SPU2 output path. `pcsx2/SPU2/spu2.cpp:121` constructs an
`AudioStream` with configured backend, driver, sync mode and stream parameters;
lines 135–139 apply volume, nominal rate, target speed and pause state. Use the
existing Cubeb or SDL backend and upstream time stretch, not the old native
port's separate PCM adapter. Failed audio initialization deliberately falls back
to a null stream and reports an error: a silent successful boot is not an audio
acceptance pass.

Queue SDL key/button events to core input processing. `Input/InputManager.h:207,
277,317` exposes host-key construction, `InvokeEvents`, and absolute pointer
updates. The frontend must implement a consistent keyboard name/code mapping;
SDL key codes and the Qt frontend's names are not automatically interchangeable.
Convert logical pointer coordinates to the units expected by the core display
translation, and account for scale/aspect/letterboxing. Release all pressed game
inputs on focus loss or when the menu captures them.

Arcade JVS and GunCon2 routes already exist. Keep the pinned title's mapping and
normal protocol flow; do not insert new game-memory writes. In this pinned
`InputManager.cpp:904–915`, generic `P1_Button2..6` bindings are explicitly
skipped. The gun trigger/pedal are configured through `USB1/guncon2_Trigger` and
`USB1/guncon2_A`; `USB/usb-lightgun/guncon2.cpp:650–680` forwards them to the
per-game ACJV mapping. Do not repeat the old reference's apparently configured
but inactive `P1_Button6` assumption.

The private reference's gun calibration was fixed through the original game
menu and verified after cold boot. Its five points live in **cabinet SRAM**, not
the dongle: the calibrated sequence appears at `sram.bin` offset `0xd8`.
Preserve per-game SRAM and owned dongle copies. Old save states embed old
calibration. The documented calibrated reference state is useful as an oracle,
not a reason to mutate native RAM or reset user storage on each launch.

## DLSS / FSR / frame generation boundary

Core presentation currently obtains `g_gs_device->GetCurrent()` and draws it via
`PresentRect` (`GSRenderer.cpp:964–983`). This supplies a resolved display image;
it does **not** establish game-linear depth, valid object motion vectors,
projection jitter, exposure, frame IDs or history-reset events.

Keep the capability gates false until those inputs and backends work. A future
GS image-handoff interface must carry texture/device identity, dimensions,
format/color space, image layout, queue ownership, synchronization, lifetime,
frame number, display/field cadence and reset status. Integrating postprocessing
inside the existing GS device avoids cross-device imports. If a second device
is retained, explicit external-memory/semaphore support and ownership transfers
are required; passing `VkImage` integers is not sufficient.

Separate the reconstructed scene image from menu/HUD composition where the
chosen backend requires it. Present generated frames without changing the EE,
IOP, VU or SPU2 cadence. Report guest/video/generated/presentation rates
separately. PCSX2 interlaced fields, repeated displays and deinterlacing make
"one present equals one game frame" an invalid default assumption.

## Next concrete implementation and acceptance gates

1. Add a frontend adapter and a new executable linked through the core CMake
   targets. Resolve every required callback, with logging and real CPU/UI
   queues. A `--boot-manifest ... --frames N` mode should fail visibly on missing
   media/resources and exit cleanly after a bounded run.
2. Hand the SDL native window to GS and disable the standalone swapchain while
   GS owns it. Boot the user's private TC3 manifest with upstream execution,
   GS/SPU2/IOP/VU defaults and the calibrated private SRAM. No selective native
   replacement yet. Validate Vulkan lifecycle under boot, resize, pause and
   shutdown, including repeated boot/shutdown cycles.
3. Connect pointer, trigger and pedal using the actual input pipeline. Verify
   JVS bits and the game's switch/aim test, then normal gameplay. The earlier
   reference automation's held-key read was zero; that is unresolved delivery
   evidence, not proof that the emulator's pedal implementation is broken.
4. Attach the Nuklear overlay to the core's presentation pass. Exercise focus
   loss, menu pause/resume, fullscreen, audio recovery and settings persistence.
5. Record stage-one playback against the calibrated reference, including the
   reported camera/freeze transition, with guest-frame and audio cadence metrics.
   Only after this baseline is correct should native block replacement or
   temporal upscaling/frame generation be enabled.

Exact current blockers are the missing linked frontend/callback implementation,
CPU/UI task queues, GS window/swapchain handoff, gameplay input adapter, resource
and persistent-config initialization, and overlay hook. The existing archive and
menu smoke test do not satisfy these gates or establish 60-FPS gameplay.
