// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "paused_preview.h"
#include <memory>

namespace tc3::preview {
namespace {
std::unique_ptr<GSTexture> source;
struct Key {
    bool fxaa, shade_boost;
    unsigned brightness, contrast, saturation, gamma;
    GSPostBilinearMode filter;
    int width, height;
    bool operator==(const Key&) const = default;
};
Key previous{};
bool prepared = false;
Key key() {
    return {GSConfig.FXAA != 0, GSConfig.ShadeBoost != 0,
        GSConfig.ShadeBoost_Brightness, GSConfig.ShadeBoost_Contrast,
        GSConfig.ShadeBoost_Saturation, GSConfig.ShadeBoost_Gamma,
        GSConfig.LinearPresent, g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight()};
}
}

void Reset() {
    // Called before GSDevice::Destroy, while GPU resource destruction is valid.
    source.reset();
    prepared = false;
}

void Capture() {
    GSTexture* image = g_gs_device->GetCurrent();
    if (!image) { Reset(); return; }
    if (!source || source->GetSize() != image->GetSize() || source->GetFormat() != image->GetFormat()) {
        source.reset(g_gs_device->CreateRenderTarget(image->GetSize(), image->GetFormat(), false, false));
        if (!source) { prepared = false; return; }
    }
    // A real GPU copy also resolves lazy-cleared images. No readback to the CPU.
    g_gs_device->StretchRect(image, source.get(), ShaderConvert::COPY, Filter::Nearest);
    previous = key();
    prepared = true; // The normal Merge finishes applying this key's effects.
}

void Prepare() {
    if (!source || !g_gs_device) return;
    const auto current_key = key();
    if (prepared && current_key == previous && g_gs_device->GetCurrent()) return;

    // Reuse the core's output-copy path without invoking GSRenderer::Merge:
    // that path can perform guest framebuffer feedback and interlace history.
    // EN2 alone copies the saved source without alpha blending or feedback.
    GSTexture* inputs[3] = {nullptr, source.get(), nullptr};
    GSVector4 src[3] = {GSVector4(0, 0, 1, 1), GSVector4(0, 0, 1, 1), GSVector4::zero()};
    GSVector4 dst[3] = {GSVector4::zero(),
        GSVector4(0, 0, source->GetWidth(), source->GetHeight()), GSVector4::zero()};
    GSRegPMODE mode{};
    mode.EN2 = 1;
    GSRegEXTBUF feedback{};
    g_gs_device->Merge(inputs, src, dst, source->GetSize(), mode, feedback, 0);
    if (GSConfig.ShadeBoost) g_gs_device->ShadeBoost();
    if (GSConfig.FXAA) g_gs_device->FXAA();
    if (GSConfig.LinearPresent == GSPostBilinearMode::BilinearSharp &&
        (g_gs_device->GetWindowWidth() > source->GetWidth() ||
         g_gs_device->GetWindowHeight() > source->GetHeight()))
        g_gs_device->Resize(g_gs_device->GetWindowWidth(), g_gs_device->GetWindowHeight());
    previous = current_key;
    prepared = true;
}
}
