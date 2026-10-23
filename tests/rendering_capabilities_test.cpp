#include "rendering/capabilities.h"
#include <cstdlib>
#include <iostream>
using namespace tc3::rendering;
static void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    for (auto c : capabilities) require(!c.available, "Unconnected host advertised a feature");
    BackendState b;
    b.linux_dlss_initialized = true;
    require(!evaluate(b)[0].available, "DLSS SDK alone is insufficient");
    b.scene_color = b.scene_depth = b.motion_vectors = b.jitter_and_reset = true;
    b.temporal_upscaler = true;
    require(evaluate(b)[0].available, "Complete DLSS contract rejected");
    require(!evaluate(b)[1].available, "DLSS must not enable FSR");
    b.motion_vectors = false;
    require(!evaluate(b)[0].available, "Missing motion vectors accepted");
    b.frame_generation_initialized = b.optical_flow = true;
    require(!evaluate(b)[9].available, "Frame generation without presenter accepted");
    b.generated_frame_presenter = true;
    require(evaluate(b)[9].available, "Image-only flow contract rejected");
    require(!evaluate(b)[8].available, "Temporal frame generation lacks motion vectors");
    b.jitter_and_reset = false;
    require(!evaluate(b)[9].available, "History reset contract missing");
    std::cout << "Rendering capability gates passed (no GPU execution tested)\n";
}
