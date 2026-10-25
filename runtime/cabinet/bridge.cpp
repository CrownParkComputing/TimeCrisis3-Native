// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental TC3 transport boundary. Original ILSOCK descriptors, receive
// queues, blocking worker threads, ILDRV handlers and SIF replies stay guest.
#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/IopMem.h"
#include "pcsx2/R3000A.h"
#include "guest_queue.h"
#include "module.h"
#include "local_transport.h"
#include "bridge.h"
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <optional>

namespace {
using namespace tc3::cabinet;
struct State {
    std::unique_ptr<LocalTransport> transport;
    const ModuleProfile* profile=nullptr;
    std::uint32_t module=0, delay=0, thbase=0;
    unsigned role=0;
    std::uint64_t sent=0,received=0,waits=0;
    Identity local{},peer{};
    std::filesystem::path peer_path;
    bool configured=false;
} state;
bool enabled() {
    static const bool value=[] {
        const char* role=std::getenv("TC3_CABINET_ROLE");
        const char* directory=std::getenv("TC3_CABINET_DIR");
        if(!role || !directory) return false;
        if(std::strcmp(role,"left")!=0 && std::strcmp(role,"right")!=0) {
            std::fprintf(stderr,"[TC3 cabinet] Invalid role; native cabinet bridge disabled\n");return false;
        }
        try {
            auto root=std::filesystem::path(directory);
            if(!root.is_absolute() || !std::filesystem::is_directory(root))
                throw std::invalid_argument("TC3_CABINET_DIR must name an existing absolute directory");
            state.role=std::strcmp(role,"right")==0 ? 2 : 1;
            state.local={0x08,0x00,0x46,0x32,0x54,0x43,0x33,static_cast<std::uint8_t>(state.role)};
            state.peer=state.local;state.peer[7]=3-state.role;
            state.peer_path=root/(state.role==1 ? "right.sock" : "left.sock");
            state.transport=std::make_unique<LocalTransport>(root/(state.role==1 ? "left.sock" : "right.sock"),state.peer_path);
            state.configured=true;
            std::fprintf(stdout,"[TC3 cabinet] EXPERIMENTAL %s transport initialized; awaiting original ILSOCK\n",role);
            return true;
        } catch(const std::exception& e) {
            std::fprintf(stderr,"[TC3 cabinet] Transport unavailable: %s\n",e.what());return false;
        }
    }();
    return value;
}
GuestRam memory() {return GuestRam({iopMem->Main,Ps2MemSize::ExposedIopRam});}
std::uint32_t word(const Identity& id,unsigned start) {
    return std::uint32_t(id[start])|std::uint32_t(id[start+1])<<8|
           std::uint32_t(id[start+2])<<16|std::uint32_t(id[start+3])<<24;
}
int finish(std::int32_t result) {
    psxRegs.GPR.n.v0=static_cast<std::uint32_t>(result);
    psxRegs.pc=psxRegs.GPR.n.ra;
    return 1;
}
std::optional<std::uint32_t> socket_address(GuestRam& ram,std::uint32_t id) {
    auto base=state.module;
    if(!(state.profile && is_original_module(ram,base,false,*state.profile))) return std::nullopt;
    auto count=ram.read32(base+0x28e0+state.profile->globals_delta),array=ram.read32(base+0x2904+state.profile->globals_delta);
    if(!array || count>64 || id>=count || !ram.valid(array,count*4)) return std::nullopt;
    auto socket=ram.read32(array+id*4);
    if(!socket || !ram.valid(socket,0x54)) return std::nullopt;
    return socket;
}
void poll_packets(GuestRam& ram) {
    GuestQueue queue(ram,state.module,*state.profile);
    for(unsigned i=0;i<64;++i) {
        auto received=state.transport->receive();
        if(received.error==std::errc::resource_unavailable_try_again || received.error==std::errc::operation_would_block) break;
        if(received.error) {
            std::fprintf(stderr,"[TC3 cabinet] Receive error: %s\n",received.error.message().c_str());break;
        }
        auto delivery=queue.inject(*received.packet);
        if(delivery.outcome==GuestQueue::Outcome::Queued) {
            ++state.received;
            if(state.received<=16 || state.received%600==0)
                std::fprintf(stdout,"[TC3 cabinet] RX #%llu bytes=%zu (guest queue; original RPC reply pending)\n",
                  static_cast<unsigned long long>(state.received),received.packet->payload.size());
            // Our narrowly intercepted original receive WaitEventFlag polls via
            // DelayThread. It re-pops this ring after sleeping; no host-generated
            // SIF reply or guest kernel event-flag mutation is needed.
        } else {
            std::fprintf(stderr,"[TC3 cabinet] Packet not queued: outcome=%u\n",static_cast<unsigned>(delivery.outcome));
        }
    }
}
int trace_init() {
    static unsigned count=0;
    if(++count<=16) std::fprintf(stdout,"[TC3 cabinet] Original Init request nsockets=%d maxpayload=%d caller=%08x\n",
       std::int32_t(psxRegs.GPR.n.a0),std::int32_t(psxRegs.GPR.n.a1),psxRegs.GPR.n.ra);
    return 0;
}
int trace_open() {
    static unsigned count=0;auto ram=memory();
    if(++count<=16) std::fprintf(stdout,"[TC3 cabinet] Original Open domain=%u type=%u protocol=%u initialized_count=%d maxpayload=%u caller=%08x\n",
       psxRegs.GPR.n.a0,psxRegs.GPR.n.a1,psxRegs.GPR.n.a2,
       state.module ? std::int32_t(ram.read32(state.module+0x28e0+state.profile->globals_delta)) : -1,
       state.module ? ram.read32(state.module+0x28e4+state.profile->globals_delta) : 0,psxRegs.GPR.n.ra);
    return 0;
}
int trace_bind() {
    static unsigned count=0;auto ram=memory();
    if(count==0 && state.module && ram.valid(state.module,state.profile->extent))
        std::fprintf(stdout,"[TC3 cabinet] Runtime ILSOCK header=%08x mode/version=%08x socketarray=%08x exports8=%08x exports14=%08x\n",
          ram.read32(state.module),ram.read32(state.module+8),ram.read32(state.module+0x2904+state.profile->globals_delta),
          ram.read32(state.module+20+8*4),ram.read32(state.module+20+14*4));
    if(++count<=16) std::fprintf(stdout,"[TC3 cabinet] Original Bind socket=%d addr=%08x len=%u valid_socket=%d caller=%08x\n",
       std::int32_t(psxRegs.GPR.n.a0),psxRegs.GPR.n.a1,psxRegs.GPR.n.a2,
       socket_address(ram,psxRegs.GPR.n.a0).has_value(),psxRegs.GPR.n.ra);
    return 0;
}
int wait_event() {
    auto ram=memory();const auto& r=psxRegs.GPR.n;
    if(!(state.profile && is_original_module(ram,state.module,false,*state.profile)) || !state.delay || r.ra!=state.module+state.profile->wait_return ||
       r.a1!=1 || r.a2!=0x11 || !ram.valid(r.s2,0x54) ||
       ram.read32(r.s2+0x50)!=r.a0) return 0;
    const auto count=ram.read32(state.module+0x28e0+state.profile->globals_delta),array=ram.read32(state.module+0x2904+state.profile->globals_delta);
    bool ours=false;
    if(count<=64 && array && ram.valid(array,count*4))
        for(std::uint32_t i=0;i<count;++i) ours|=ram.read32(array+4*i)==r.s2;
    if(!ours) return 0;
    poll_packets(ram);
    const auto write=ram.read32(r.s2+0x44),read=ram.read32(r.s2+0x48);
    if(write!=read) return finish(0);
    ++state.waits;
    // Original guest DelayThread performs the yield and scheduler work. It
    // returns zero to ILSOCK's unmodified loop, which checks its queue again.
    psxRegs.GPR.n.a0=500;
    psxRegs.pc=state.delay;
    return 1;
}
int connect_socket() {
    auto ram=memory();auto& r=psxRegs.GPR.n;
    auto socket=socket_address(ram,r.a0);
    if(!socket) return finish(-2004);
    if(!r.a1 || r.a2<12 || !ram.valid(r.a1,12)) return finish(-2006);
    auto p=iopMem->Main+(r.a1&0x1fffffffU);
    if(p[1]!=0x3c && p[1]!=0) return finish(-2006);
    if(p[1]==0) {
        ram.write32(*socket+4,0xffffffff);ram.write32(*socket+12,0);ram.write32(*socket+16,0);
        ram.write32(*socket,ram.read32(*socket)&~2U);return finish(0);
    }
    auto remote=decode_address({p,12});
    const auto all_zero=Identity{},broadcast=Identity{255,255,255,255,255,255,255,255};
    if(remote->identity==all_zero) remote->identity=state.local;
    if(remote->identity!=broadcast && remote->identity!=state.local && remote->identity!=state.peer)
        return finish(-1021);
    ram.write32(*socket+4,std::uint32_t(remote->port[0])|std::uint32_t(remote->port[1])<<8);
    ram.write32(*socket+12,word(remote->identity,0));ram.write32(*socket+16,word(remote->identity,4));
    ram.write32(*socket,ram.read32(*socket)|2U);
    std::fprintf(stdout,"[TC3 cabinet] Original socket %u connected via native local transport\n",r.a0);
    return finish(0);
}
int send_common(bool to) {
    auto ram=memory();auto& r=psxRegs.GPR.n;
    auto socket=socket_address(ram,r.a0);
    if(!socket) return finish(-2004);
    auto maximum=ram.read32(state.module+0x28e4+state.profile->globals_delta);
    if(r.a2>maximum || r.a2>default_max_payload || !ram.valid(r.a1,r.a2)) return finish(-2007);
    Packet packet;
    if(to) {
        if(!ram.valid(r.sp+16,8)) return finish(-2006);
        auto address=ram.read32(r.sp+16),length=ram.read32(r.sp+20);
        if(!address || length<12 || !ram.valid(address,12)) return finish(-2006);
        auto decoded=decode_address({iopMem->Main+(address&0x1fffffffU),12});
        if(!decoded) return finish(-2006);
        packet.destination=*decoded;
    } else {
        auto remote=ram.read32(*socket+4);
        if(std::int32_t(remote)<0) return finish(-2005);
        packet.destination.port={static_cast<std::uint8_t>(remote),static_cast<std::uint8_t>(remote>>8)};
        auto guid0=ram.read32(*socket+12),guid1=ram.read32(*socket+16);
        for(unsigned i=0;i<4;++i) {packet.destination.identity[i]=guid0>>(8*i);packet.destination.identity[i+4]=guid1>>(8*i);}
    }
    const Identity broadcast{255,255,255,255,255,255,255,255};
    if(packet.destination.identity!=state.local && packet.destination.identity!=state.peer &&
       packet.destination.identity!=broadcast) return finish(-1021);
    auto local=ram.read32(*socket+8);
    if(std::int32_t(local)<0) {
        // Match original ephemeral allocation: raw LE word contains network-order port.
        for(unsigned attempt=0;attempt<0x4000;++attempt) {
            auto counter=ram.read32(state.module+0x2900+state.profile->globals_delta);
            auto port=0xc000U+(counter&0x3fffU);ram.write32(state.module+0x2900+state.profile->globals_delta,counter+1);
            local=((port&255)<<8)|(port>>8);
            bool taken=false;
            auto count=ram.read32(state.module+0x28e0+state.profile->globals_delta),array=ram.read32(state.module+0x2904+state.profile->globals_delta);
            for(unsigned i=0;i<count;++i) {
                auto s=ram.read32(array+4*i);
                if(s && ram.valid(s,12) && ram.read32(s+8)==local) taken=true;
            }
            if(!taken) break;
        }
        ram.write32(*socket+8,local);
    }
    packet.source.port={static_cast<std::uint8_t>(local),static_cast<std::uint8_t>(local>>8)};
    packet.source.identity=state.local;
    auto source=iopMem->Main+(r.a1&0x1fffffffU);
    packet.payload.assign(source,source+r.a2);
    if(packet.destination.identity==state.local) {
        GuestQueue queue(ram,state.module,*state.profile);
        auto delivered=queue.inject(packet);
        return finish(delivered.outcome==GuestQueue::Outcome::Queued ? r.a2 : 0);
    }
    auto error=state.transport->send(packet);
    if(error) {
        if(error==std::errc::resource_unavailable_try_again || error==std::errc::no_buffer_space) return finish(0);
        std::fprintf(stderr,"[TC3 cabinet] Send failure: %s\n",error.message().c_str());return finish(-1021);
    }
    ++state.sent;
    if(state.sent<=16 || state.sent%600==0)
        std::fprintf(stdout,"[TC3 cabinet] TX #%llu bytes=%u\n",static_cast<unsigned long long>(state.sent),r.a2);
    return finish(static_cast<std::int32_t>(r.a2));
}
int send_socket() {return send_common(false);}
int send_to_socket() {return send_common(true);}
int eui64() {
    auto ram=memory();auto target=psxRegs.GPR.n.a0;
    if(!target || !ram.valid(target,8)) return finish(-1006);
    // Original ILSOCK swaps each EUI64 word after sce1394SbEui64; invert that
    // swap here so its cached/wire identity is exactly state.local.
    std::array<std::uint8_t,8> raw{};
    for(unsigned i=0;i<8;++i) raw[i]=state.local[(i&~3U)+(3-(i&3U))];
    ram.write(target,raw);return finish(0);
}
int node_id() {return finish(0xffc0|state.role);}
int node_count() {return finish(std::filesystem::exists(state.peer_path) ? 2 : 1);}
} // namespace

TC3CabinetNativeImport tc3CabinetImportHLE(const std::string& library,std::uint16_t index) {
    if(!enabled()) return nullptr;
    if(library=="thevent" && index==10) return wait_event;
    if(library=="ilink") {
        if(index==27) return eui64;
        if(index==28) return node_id;
        if(index==29) return node_count;
    }
    if(library=="ilsocket") {
        if(index==4) return trace_init;
        if(index==8) return trace_open;
        if(index==10) return trace_bind;
        if(index==11) return connect_socket;
        if(index==12) return send_socket;
        if(index==13) return send_to_socket;
    }
    return nullptr;
}
void tc3CabinetRegisterExports(std::uint32_t table) {
    if(!enabled()) return;
    auto ram=memory();if(!ram.valid(table,20)) return;
    auto name=iopMemReadString(table+12,8);
    if(name=="thbase" && ram.valid(table,20+34*4)) {
        auto target=ram.read32(table+20+33*4);
        if(target && ram.valid(target,8)) {state.delay=target;state.thbase=table;}
    }
    if(name=="ilsocket") {
        const auto* profile=identify_module(ram,table,true);
        if(!profile) {
            std::fprintf(stderr,"[TC3 cabinet] ILSOCK signature mismatch; transport guest adapter rejected\n");return;
        }
        state.module=table;state.profile=profile;
        std::fprintf(stdout,"[TC3 cabinet] Original ILSOCK verified at %08x; guest DelayThread=%08x; profile=%s\n",table,state.delay,profile->name);
    }
}
void tc3CabinetReleaseExports(std::uint32_t table) {
    if(table==state.module) {state.module=0;state.profile=nullptr;}
    if(table==state.thbase) {state.thbase=0;state.delay=0;}
}
