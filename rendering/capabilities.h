#pragma once
#include <array>

namespace tc3::rendering {
struct Capability {
    const char* name;
    bool available;
    const char* reason;
};

// Availability requires a working backend AND the frame inputs it consumes.
// Device vendor/extension detection alone must never turn a menu option on.
struct BackendState {
    bool scene_color = false;
    bool scene_depth = false;
    bool motion_vectors = false;
    bool jitter_and_reset = false;
    bool temporal_upscaler = false;
    bool linux_dlss_initialized = false;
    bool fsr3_initialized = false;
    bool fsr4_initialized = false;
    bool fsr411_initialized = false;
    bool taa_initialized = false;
    bool rcas_initialized = false;
    bool frame_generation_initialized = false;
    bool generated_frame_presenter = false;
    bool optical_flow = false;
    bool object_motion = false;
};

constexpr auto evaluate(const BackendState& b) {
    const bool temporal = b.scene_color && b.scene_depth && b.motion_vectors && b.jitter_and_reset;
    const bool upscale = temporal && b.temporal_upscaler;
    const bool generation = b.scene_color && b.jitter_and_reset &&
        b.frame_generation_initialized && b.generated_frame_presenter;
    return std::array{
        Capability{"DLSS Super Resolution", upscale && b.linux_dlss_initialized,
                   "Needs Linux DLSS integration and validated scene inputs"},
        Capability{"FSR 3", upscale && b.fsr3_initialized,
                   "Needs FSR integration and validated scene inputs"},
        Capability{"FSR 4", upscale && b.fsr4_initialized,
                   "Needs a supported FSR 4 backend and scene inputs"},
        Capability{"FSR 4.1.1", upscale && b.fsr411_initialized,
                   "Needs a supported FSR 4.1.1 backend and scene inputs"},
        Capability{"Temporal AA", temporal && b.taa_initialized,
                   "Needs temporal AA integration and scene inputs"},
        Capability{"Quality preset", upscale && (b.linux_dlss_initialized ||
                   b.fsr3_initialized || b.fsr4_initialized || b.fsr411_initialized),
                   "Native AA / Quality / Balanced / Performance / Ultra Performance"},
        Capability{"RCAS sharpening / sharpness", b.scene_color && b.rcas_initialized,
                   "Needs a connected sharpening pass"},
        Capability{"Subpixel jitter", temporal && b.temporal_upscaler,
                   "Needs scene projection and temporal history integration"},
        Capability{"Frame generation", generation && temporal,
                   "Needs frame interpolation, scene inputs and presentation integration"},
        Capability{"Frame generation: image only", generation && b.optical_flow,
                   "Needs optical flow and presentation integration"},
        Capability{"Object motion vectors", b.scene_color && b.object_motion,
                   "Needs verified TC3 object transforms and previous-frame history"}
    };
}

// The initial host has no game renderer attached. These are deliberately disabled.
inline constexpr auto capabilities = evaluate(BackendState{});
} // namespace tc3::rendering
