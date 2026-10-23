// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/cabinet/guest_queue.h"
#include "runtime/cabinet/module.h"
#include <cstdio>
#include <cstdlib>
using namespace tc3::cabinet;
static void require(bool value,const char* why) {
    if(!value) {std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}
}
int main() {
    constexpr unsigned base=0x10000,array=0x20000,socket=0x21000,node=0x22000,data=0x23000;
    std::vector<std::uint8_t> bytes(0x200000);
    GuestRam ram(bytes);
    ram.write32(base+0x28e0,2);ram.write32(base+0x28e4,492);
    ram.write32(base+0x2904,array);ram.write32(base+0x2918,0x04030201);ram.write32(base+0x291c,0x08070605);
    ram.write32(array,socket);ram.write32(socket+4,0xffffffff);ram.write32(socket+8,0x3412);
    ram.write32(socket+0x4c,8);ram.write32(socket+0x50,17);
    ram.write32(base+0x2920,node);ram.write32(node+0x20,data);
    Packet packet{{{0x12,0x34},{1,2,3,4,5,6,7,8}},{{0xab,0xcd},{8,7,6,5,4,3,2,1}},{0,0xff,0x20,0x30}};
    GuestQueue queue(ram,base);
    auto saved=bytes;
    auto foreign=packet;foreign.destination.identity.fill(0);
    require(queue.inject(foreign).outcome==GuestQueue::Outcome::ForeignDestination && bytes==saved,"foreign packet leaves guest RAM unchanged");
    auto wrongport=packet;wrongport.destination.port={0,1};
    require(queue.inject(wrongport).outcome==GuestQueue::Outcome::NoSocket && bytes==saved,"unbound destination leaves guest RAM unchanged");
    auto result=queue.inject(packet);
    require(result.outcome==GuestQueue::Outcome::Queued && result.needs_wake && result.wake_event==17,"empty queue requests original guest eventflag");
    require(ram.read32(socket+0x24)==node && ram.read32(socket+0x44)==1,"original queue write index advances");
    require(ram.read32(node+8)==4 && ram.read32(base+0x2920)==0,"original free-list and payload size updated");
    auto wire=encode_packet(packet);
    require(std::equal(wire->begin(),wire->begin()+20,bytes.begin()+node+12),"packet header matches original pool node layout");
    require(std::equal(packet.payload.begin(),packet.payload.end(),bytes.begin()+data),"binary payload preserved");
    saved=bytes;
    require(queue.inject(packet).outcome==GuestQueue::Outcome::NoBuffers && bytes==saved,"exhausted guest pool does not mutate RAM");
    std::fill(bytes.begin(),bytes.end(),0);ram.write32(base+0x28e0,2);ram.write32(base+0x28e4,492);
    ram.write32(base+0x2904,array);ram.write32(base+0x2918,0x04030201);ram.write32(base+0x291c,0x08070605);
    ram.write32(array,socket);ram.write32(socket+4,0xffffffff);ram.write32(socket+8,0x3412);ram.write32(socket+0x4c,8);ram.write32(socket+0x44,7);ram.write32(socket+0x50,17);
    saved=bytes;
    require(queue.inject(packet).outcome==GuestQueue::Outcome::Full && bytes==saved,"ring reserves one slot, matching guest seven-packet capacity");
    ram.write32(socket+0x44,8);saved=bytes;
    require(queue.inject(packet).outcome==GuestQueue::Outcome::InvalidState && bytes==saved,"corrupt guest queue rejected without writes");
    require(ram.valid(0x80000000+base,4) && ram.valid(0xa0000000+base,4) &&
            !ram.valid(0x1ffffffe,4) && !ram.valid(0xc0000000+base,4) && !ram.valid(0x40000000+base,4),
            "only normal KSEG aliases supported; invalid RAM never wraps");
    // Small instruction/export fixture extracted from original ILSOCK bytes.
    // Headers were observed at actual relocated base0x64330 in paired boot.
    constexpr std::pair<unsigned,unsigned> fixture[]={{0,0x41c00000},{8,0x202},
      {20+8*4,base+0x1448},{20+14*4,base+0x238c},{0x214c,0x27bdffc8},
      {0x2194,0x8e440050},{0x2198,0x24060011},{0x21a0,0x27a70010},{0x21a4,0x0440002c}};
    for(auto [offset,value]:fixture) ram.write32(base+offset,value);
    require(is_original_module(ram,base,true),"original relocated disk-header fixture admitted at registration");
    ram.write32(base,0x000c9cc0);ram.write32(base+4,0x00061dc4);
    require(!is_original_module(ram,base,true) && is_original_module(ram,base),
            "observed loadcore library links admitted only in runtime validation");
    ram.write32(base+8,0x00010202);
    require(is_original_module(ram,base),"loader-owned mode bits do not alter module identity");
    ram.write32(base+0x2194,0);
    require(!is_original_module(ram,base),"changed receive instructions rejected after registration");
    ram.write32(base+0x2194,0x8e440050);ram.write32(base+20+14*4,base+0x2400);
    require(!is_original_module(ram,base),"changed relocated export target rejected");
    ram.write32(base+20+14*4,base+0x238c);ram.write32(base+8,0x203);
    require(!is_original_module(ram,base),"different module version rejected");
    std::puts("PASS original guest ILSOCK receive pool/ring; deferred eventflag wake; packet filtering; full/exhausted/corrupt queue invariants");
}
