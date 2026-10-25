// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
namespace tc3::cabinet {
// Original ILSOCK 2.2 revisions. Values are verified IRX-relative addresses,
// not game RAM patches. Descriptor/ring/wire ABI is shared by these revisions.
struct ModuleProfile {
    const char* name;
    std::uint32_t extent, open_export, recv_export, receive_entry, wait_return, globals_delta;
};
inline constexpr ModuleProfile tc3_profile{"TC3 TST3",0x2930,0x1448,0x238c,0x214c,0x21a4,0};
inline constexpr ModuleProfile ridge_profile{"Ridge V RRV3/A",0x29d0,0x1410,0x2428,0x21e8,0x2240,0xa0};
inline constexpr std::array profiles{tc3_profile,ridge_profile};
}
