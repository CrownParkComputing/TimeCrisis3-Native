// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "guest_queue.h"
namespace tc3::cabinet {
// TST3 ILSOCK 2.2. Export entries have already been relocated by loadcore.
// Its first two disk-header words become library-list pointers once registered.
// Mode bits share the upper half of the version word and are loader-owned too.
inline bool is_original_module(GuestRam& ram,std::uint32_t base,bool registration=false,
                               const ModuleProfile& profile=tc3_profile) {
    if(!base || !ram.valid(base,profile.extent)) return false;
    const auto wait=profile.wait_return;
    return (!registration || ram.read32(base)==0x41c00000) && (ram.read32(base+8)&0xffff)==0x202 &&
           ram.read32(base+20+8*4)==base+profile.open_export &&
           ram.read32(base+20+14*4)==base+profile.recv_export &&
           ram.read32(base+profile.receive_entry)==0x27bdffc8 &&
           ram.read32(base+wait-0x10)==0x8e440050 &&
           ram.read32(base+wait-0xc)==0x24060011 &&
           ram.read32(base+wait-4)==0x27a70010 &&
           ram.read32(base+wait)==0x0440002c;
}
inline const ModuleProfile* identify_module(GuestRam& ram,std::uint32_t base,bool registration=true) {
    for(const auto& profile:profiles)
        if(is_original_module(ram,base,registration,profile)) return &profile;
    return nullptr;
}
} // namespace tc3::cabinet
