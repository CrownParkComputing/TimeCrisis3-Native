// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "guest_queue.h"
namespace tc3::cabinet {
// TST3 ILSOCK 2.2. Export entries have already been relocated by loadcore.
// Its first two disk-header words become library-list pointers once registered.
// Mode bits share the upper half of the version word and are loader-owned too.
inline bool is_original_module(GuestRam& ram,std::uint32_t base,bool registration=false) {
    if(!base || !ram.valid(base,0x2930)) return false;
    return (!registration || ram.read32(base)==0x41c00000) && (ram.read32(base+8)&0xffff)==0x202 &&
           ram.read32(base+20+8*4)==base+0x1448 &&
           ram.read32(base+20+14*4)==base+0x238c &&
           ram.read32(base+0x214c)==0x27bdffc8 &&
           ram.read32(base+0x2194)==0x8e440050 &&
           ram.read32(base+0x2198)==0x24060011 &&
           ram.read32(base+0x21a0)==0x27a70010 &&
           ram.read32(base+0x21a4)==0x0440002c;
}
} // namespace tc3::cabinet
