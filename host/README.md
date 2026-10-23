# Linux host

`tc3-native-host` is an SDL3 window with a direct Vulkan renderer for Nuklear
triangles and the font atlas. It does not execute Time Crisis 3 yet. It never
reports menu frame rate as gameplay performance.

Dependencies: C++20 compiler, CMake 3.24+, pkg-config, SDL3, Vulkan headers/loader,
and `glslangValidator`. Shaders are compiled into the binary at build time;
there are no runtime shader-path dependencies. The default Nuklear font is
embedded in its licensed header.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tc3-native-host
./build/host/tc3-native-host
```

The Display page's fullscreen and presentation-rate controls operate on the
host. VSync uses Vulkan FIFO. Rendering and Performance pages list the requested
DriveClub-equivalent features from `rendering/capabilities.h`; unavailable
backends have disabled controls and an explanation. DLSS, FSR, TAA, RCAS, jitter,
frame generation, and object motion vectors are not implemented by this menu.

DriveClub's local `runtime-linux/gpu/shim/nk_menu.cpp` was inspected: it converts
Nuklear geometry into ImGui draw lists for its Vulkan presenter. This host instead
uses its own Vulkan pipeline and does not bring in that ImGui bridge. The only
copied dependency is the unmodified Nuklear header; see its `ORIGIN.txt`.

## GPU validation

With a desktop session and `VK_LAYER_KHRONOS_validation` installed:

```sh
./build/host/tc3-native-host --smoke-frames 12 --smoke-resize --smoke-ui --validation
```

This bounded run renders all four pages, recreates the swapchain, enters and
leaves fullscreen, and fails on validation errors or a 30-second timeout. It
requires a real Vulkan surface; it is not a headless emulation/gameplay test.
Opt into the same check in CTest with `-DTC3_ENABLE_GPU_TESTS=ON`.

The initial shell deliberately uses one frame in flight and waits for its queue
before recycling buffers. Guest presentation requires its own frame scheduling
integration; this shell's presentation throughput is not a game benchmark.
