// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/cabinet/module.h"
#include <cstdlib>
#include <iostream>
using namespace tc3::cabinet;
void check(bool b){if(!b)std::abort();}
int main(){
    constexpr unsigned base=0x10000,sockets=0x20000,socket=0x21000,node=0x22000,data=0x23000;
    for(const auto& profile:profiles){
        std::vector<std::uint8_t> bytes(0x200000);GuestRam ram(bytes);
        ram.write32(base,0x41c00000);ram.write32(base+8,0x202);
        ram.write32(base+20+8*4,base+profile.open_export);ram.write32(base+20+14*4,base+profile.recv_export);
        ram.write32(base+profile.receive_entry,0x27bdffc8);ram.write32(base+profile.wait_return-0x10,0x8e440050);
        ram.write32(base+profile.wait_return-0xc,0x24060011);ram.write32(base+profile.wait_return-4,0x27a70010);
        ram.write32(base+profile.wait_return,0x0440002c);
        const auto* identified=identify_module(ram,base);
        check(identified&&identified->globals_delta==profile.globals_delta);
        check(!is_original_module(ram,base,true,profile.globals_delta?tc3_profile:ridge_profile));
        ram.write32(base,0x123000);ram.write32(base+4,0x124000);
        check(!identify_module(ram,base,true)&&identify_module(ram,base,false));
        ram.write32(base+8,0x10202);check(is_original_module(ram,base,false,profile));
        ram.write32(base+profile.wait_return-0x10,0);check(!identify_module(ram,base,false));
        ram.write32(base+profile.wait_return-0x10,0x8e440050);
        const auto g=[&](unsigned offset){return base+offset+profile.globals_delta;};
        ram.write32(g(0x28e0),3);ram.write32(g(0x28e4),profile.globals_delta?288:492);
        ram.write32(g(0x2904),sockets);ram.write32(g(0x2918),0x04030201);ram.write32(g(0x291c),0x08070605);
        ram.write32(sockets,socket);ram.write32(socket+4,0xffffffff);ram.write32(socket+8,0x3412);
        ram.write32(socket+0x4c,8);ram.write32(socket+0x50,17);ram.write32(g(0x2920),node);ram.write32(node+0x20,data);
        Packet packet{{{0x12,0x34},{1,2,3,4,5,6,7,8}},{{0xab,0xcd},{8,7,6,5,4,3,2,1}},std::vector<std::uint8_t>(300,0x5a)};
        GuestQueue queue(ram,base,profile);auto result=queue.inject(packet);
        check(result.outcome==GuestQueue::Outcome::Queued);
        check(ram.read32(socket+0x44)==1&&ram.read32(node+8)==(profile.globals_delta?288:300));
        check(ram.read32(g(0x2920))==0&&ram.read32(data)==0x5a5a5a5a);
    }
    std::cout<<"PASS TC3/Ridge ILSOCK guards, revision isolation, relocated headers and profiled receive queues\n";
}
