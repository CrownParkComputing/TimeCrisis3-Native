// SPDX-License-Identifier: GPL-3.0-or-later
#include "rendering/options.h"
#include <cstdlib>
#include <iostream>
using namespace tc3::rendering;
void check(bool value) { if (!value) std::abort(); }
int main() {
    Options r;
    check(adjust(r, Option::Resolution, 1).resolution == 1);
    r.ready = true;
    for (int i = 0; i < 10; ++i) r = adjust(r, Option::Resolution, 1);
    check(r.resolution == 4);
    for (int i = 0; i < 10; ++i) r = adjust(r, Option::Resolution, -1);
    check(r.resolution == 1);
    check(adjust(r, Option::CAS, 1).cas == 0);
    r.cas_supported = true;
    r = adjust(r, Option::CAS, -1); check(r.cas == 2);
    for (int i = 0; i < 20; ++i) r = adjust(r, Option::Sharpness, 1);
    check(r.sharpness == 100);
    for (int i = 0; i < 20; ++i) r = adjust(r, Option::Sharpness, -1);
    check(r.sharpness == 0);
    r = adjust(r, Option::CAS, 1); check(r.cas == 0);
    check(adjust(r, Option::Sharpness, 1).sharpness == 0);
    for (int i = 0; i < 10; ++i) r = adjust(r, Option::QueueDepth, 1);
    check(r.queue_depth == 3);
    for (int i = 0; i < 10; ++i) r = adjust(r, Option::QueueDepth, -1);
    check(r.queue_depth == 1);
    r = adjust(r, Option::FXAA, 1); check(r.fxaa);
    r = adjust(r, Option::FXAA, -1); check(!r.fxaa);
    const auto encoded = pack(r);
    auto decoded = unpack(encoded);
    check(decoded && decoded->resolution == r.resolution && decoded->fxaa == r.fxaa &&
          decoded->cas == r.cas && decoded->sharpness == r.sharpness &&
          decoded->vsync == r.vsync && decoded->queue_depth == r.queue_depth);
    check(!unpack(0) && !unpack(encoded | 0x200000U));
    check(!unpack(encoded | (3U<<16)));
    check(!unpack(encoded & ~(7U<<18)));
    r=adjust(r,Option::SindenBorder,1); check(r.sinden_border==2);
    for(int i=0;i<10;++i) r=adjust(r,Option::BorderWidth,1);
    check(r.border_width==5);
    const auto border=unpack(pack(r));
    check(border && border->sinden_border==2 && border->border_width==5);
    check(!unpack((encoded & ~7U) | 7U));
    check(!unpack(encoded & ~(3U << 14)));
    std::cout << "Rendering option bounds and device capability checks passed\n";
}
