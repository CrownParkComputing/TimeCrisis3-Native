# Requested rendering parity

The playable host now connects **internal resolution (1–4x), FXAA, CAS sharpening
and strength, VSync, and queue depth** to the existing Vulkan renderer. These
are real core settings, mirrored atomically to the second cabinet and persisted.
CAS is Contrast Adaptive Sharpening; it does not claim RCAS or temporal FSR.
Live menu tests verify changes reach both GS configurations, then restore them.
The remaining temporal capabilities below are still disabled.

Source inspected: local `DriveClub-Linux/runtime-linux/gpu`, notably
`shim/nk_menu.cpp`, `shim/bbport_settings.h`, and
`shadps4/video_core/renderer_vulkan/vk_{dlss,frame_generation,temporal_upscaler}.{h,cpp}`.

| Requested control | TC3 integration required |
| --- | --- |
| Fullscreen, resolution, presentation cap, FPS overlay | SDL window/swapchain; preserve TC3 aspect ratio and pointer mapping |
| DLSS Super Resolution | Linux NGX/Vulkan backend, supported device, scene color/depth/motion, jitter and history reset |
| FSR 3 / FSR 4 / FSR 4.1.1 | Corresponding initialized Vulkan provider and validated scene inputs |
| Native AA / Quality / Balanced / Performance / Ultra Performance | Provider-specific render/output sizing and feature recreation |
| RCAS and sharpness | Actual post-process dispatch |
| Subpixel jitter | TC3 projection integration; HUD and light-gun coordinates remain stable |
| Frame generation | Interpolation dispatch, image lifetime/fences, generated-frame presenter and correct pacing |
| Image-only frame generation | Optical flow path, explicit history resets and presenter |
| Object motion vectors | Recovered current/previous object transforms; camera motion alone is insufficient |
| Motion blur | TC3-specific effect integration; DriveClub's patch is not portable |

The local DriveClub menu **skips DLSS** (`nk_menu.cpp`, `rend_change`) and its
bridge CMake explicitly requires Windows x64. Its presence in source does not
establish working Linux DLSS. Frame generation uses FSR-Vulkan, not the DLSS
bridge. Integration into TC3 is outstanding; the capability model defaults off.

DriveClub's camera/object buffer extraction and shader hashes are game-specific.
TC3 does not have the same G-buffer or TAA resolve. Reusing its Vulkan SDK provider
does not supply those inputs. Reconstruct and validate TC3's scene depth, motion,
projection and HUD separation before enabling temporal controls.

Do not transplant DriveClub's selectable 480 Hz vblank. PCSX2 owns TC3 guest timing.
The performance display must distinguish guest fields/s, real rendered frames/s,
generated frames/s and presented frames/s. Frame generation must never satisfy the
base-speed acceptance gate or alter input/guest execution cadence.

On pause, savestate load, camera cut, resize, renderer change or discontinuous field
sequence: invalidate temporal history. Render Nuklear after interpolation so its
text and mouse pointer are not interpolated. Preserve input mapping across native,
upscaled, letterboxed and fullscreen output.

Acceptance includes camera cuts, muzzle flashes, cover transitions, HUD edges,
gun hit locations, output resize, provider failure and device capability changes.
Menu capability tests only validate gating; they do not validate image quality or
demonstrate that any upscaler/frame-generation backend runs.
