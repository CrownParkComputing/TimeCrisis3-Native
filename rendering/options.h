// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace tc3::rendering {
enum class Option { Resolution, FXAA, CAS, Sharpness, VSync, QueueDepth, SindenBorder, BorderWidth,
                    Widescreen, DisplaySize, FrameSkip, DLSS };

// Published from the GS thread, after the core has applied the setting.
struct Options {
    bool ready = false;
    bool cas_supported = false;
    int resolution = 1;
    bool fxaa = false;
    int cas = 0;
    int sharpness = 50;
    bool vsync = false;
    int queue_depth = 2;
    int sinden_border = 1; // Off, Auto (USB detected), On.
    int border_width = 2; // Percent of the shorter game-view dimension.
    int aspect = 0;       // 0 = 4:3, 1 = 16:9, 2 = Stretch fill.
    int display_size = 0; // 0 = Fit window, 1 = 960x720, 2 = 1280x960, 3 = 1920x1440.
    int frame_skip = 0;   // 0 = off; 1-3 = present only every (n+1)th frame.
    int dlss = 0;         // 0 = Off, 1 = Quality, 2 = Balanced, 3 = Performance.
    bool dlss_supported = false; // NVIDIA GPU present; upscaler backend still gates it.
};

// Shared by menu and host, so keyboard, mouse and persisted values have the
// same bounds. These controls never change the guest's clock or speed limit.
inline Options adjust(Options value, Option option, int direction) {
    if (!value.ready || direction == 0) return value;
    const int step = direction < 0 ? -1 : 1;
    switch (option) {
    case Option::Resolution:
        value.resolution = std::clamp(value.resolution + step, 1, 4); break;
    case Option::FXAA: value.fxaa = !value.fxaa; break;
    case Option::CAS:
        if (value.cas_supported) value.cas = (value.cas + step + 3) % 3;
        break;
    case Option::Sharpness:
        if (value.cas_supported && value.cas != 0)
            value.sharpness = std::clamp(value.sharpness + step * 10, 0, 100);
        break;
    case Option::VSync: value.vsync = !value.vsync; break;
    case Option::SindenBorder: value.sinden_border = (value.sinden_border + step + 3) % 3; break;
    case Option::BorderWidth: value.border_width = std::clamp(value.border_width + step, 1, 5); break;
    case Option::QueueDepth:
        value.queue_depth = std::clamp(value.queue_depth + step, 1, 3); break;
    case Option::Widescreen: value.aspect = (value.aspect + step + 3) % 3; break;
    case Option::DisplaySize: value.display_size = (value.display_size + step + 4) % 4; break;
    case Option::FrameSkip: value.frame_skip = (value.frame_skip + step + 4) % 4; break;
    case Option::DLSS:
        if (value.dlss_supported) value.dlss = (value.dlss + step + 4) % 4;
        break;
    }
    return value;
}

// One atomic word publishes the complete settings transaction to the peer.
// No guest timing, addresses or pointers cross this control channel.
inline std::uint32_t pack(const Options& r) {
    if (!r.ready) return 0;
    return 0x80000000U | std::clamp(r.resolution, 1, 4) | (std::uint32_t(r.fxaa) << 3) |
        (std::uint32_t(std::clamp(r.cas, 0, 2)) << 4) |
        (std::uint32_t(std::clamp(r.sharpness, 0, 100)) << 6) |
        (std::uint32_t(r.vsync) << 13) | (std::uint32_t(std::clamp(r.queue_depth, 1, 3)) << 14) |
        (std::uint32_t(std::clamp(r.sinden_border, 0, 2)) << 16) |
        (std::uint32_t(std::clamp(r.border_width, 1, 5)) << 18) |
        (std::uint32_t(std::clamp(r.aspect, 0, 2)) << 21) |
        (std::uint32_t(std::clamp(r.display_size, 0, 3)) << 23) |
        (std::uint32_t(std::clamp(r.frame_skip, 0, 3)) << 25) |
        (std::uint32_t(std::clamp(r.dlss, 0, 3)) << 27);
}
inline std::optional<Options> unpack(std::uint32_t bits) {
    if ((bits & 0xffe00000U) != 0x80000000U) return std::nullopt;
    Options r;
    r.ready = true;
    r.resolution = bits & 7;
    r.fxaa = (bits >> 3) & 1;
    r.cas = (bits >> 4) & 3;
    r.sharpness = (bits >> 6) & 127;
    r.vsync = (bits >> 13) & 1;
    r.queue_depth = (bits >> 14) & 3;
    r.sinden_border = (bits >> 16) & 3;
    r.border_width = (bits >> 18) & 7;
    r.aspect = (bits >> 21) & 3;
    r.display_size = (bits >> 23) & 3;
    r.frame_skip = (bits >> 25) & 3;
    r.dlss = (bits >> 27) & 3;
    if (r.resolution < 1 || r.resolution > 4 || r.cas > 2 || r.sharpness > 100 || r.queue_depth < 1 ||
        r.sinden_border > 2 || r.border_width < 1 || r.border_width > 5 || r.aspect > 2 ||
        r.display_size > 3 || r.frame_skip > 3 || r.dlss > 3)
        return std::nullopt;
    return r;
}
} // namespace tc3::rendering
