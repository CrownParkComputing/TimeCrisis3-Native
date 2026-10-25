// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "packet.h"
#include "profile.h"
#include <span>
#include <stdexcept>

namespace tc3::cabinet {
// View used by the opt-in IOP adapter; addresses are normalized only across
// the normal KSEG aliases. Never wrap an out-of-range access into IOP RAM.
class GuestRam {
public:
    explicit GuestRam(std::span<std::uint8_t> memory): memory_(memory) {}
    bool valid(std::uint32_t address,std::size_t bytes) const {
        const auto segment=address & 0xe0000000U;
        if(segment!=0 && segment!=0x80000000U && segment!=0xa0000000U) return false;
        auto p=address & 0x1fffffffU;
        return p<=memory_.size() && bytes<=memory_.size()-p;
    }
    std::uint32_t read32(std::uint32_t address) const {
        if(!valid(address,4)) throw std::out_of_range("IOP cabinet read");
        auto p=memory_.data()+(address&0x1fffffffU);
        return std::uint32_t(p[0])|std::uint32_t(p[1])<<8|std::uint32_t(p[2])<<16|std::uint32_t(p[3])<<24;
    }
    void write32(std::uint32_t address,std::uint32_t value) {
        std::array<std::uint8_t,4> bytes{std::uint8_t(value),std::uint8_t(value>>8),std::uint8_t(value>>16),std::uint8_t(value>>24)};
        write(address,bytes);
    }
    void write(std::uint32_t address,std::span<const std::uint8_t> bytes) {
        if(!valid(address,bytes.size())) throw std::out_of_range("IOP cabinet write");
        std::copy(bytes.begin(),bytes.end(),memory_.begin()+(address&0x1fffffffU));
    }
private:
    std::span<std::uint8_t> memory_;
};

// Original TST3 ILSOCK callback RVA0x1330, queue helpers 0x1154/0x11fc,
// socket lookup0x968. Call only on the CPU/IOP owner while guest interrupts
// cannot mutate this queue. A returned event must be signalled through the
// original guest kernel. This object never executes or invents RPC completion.
class GuestQueue {
public:
    enum class Outcome { Queued, ForeignDestination, NoSocket, Full, NoBuffers, InvalidState };
    struct Delivery {
        Outcome outcome;
        std::uint32_t wake_event=0;
        bool needs_wake=false;
    };
    GuestQueue(GuestRam& ram,std::uint32_t module_base,const ModuleProfile& profile=tc3_profile)
        :ram_(ram),base_(module_base),profile_(profile) {}
    Delivery inject(const Packet& packet) {
        if(!ram_.valid(base_,profile_.extent)) return {Outcome::InvalidState};
        const auto nsockets=ram_.read32(base_+0x28e0+profile_.globals_delta), array=ram_.read32(base_+0x2904+profile_.globals_delta);
        const auto maximum=ram_.read32(base_+0x28e4+profile_.globals_delta);
        if(nsockets==0 || nsockets>64 || !array || !ram_.valid(array,nsockets*4) ||
           maximum>default_max_payload) return {Outcome::InvalidState};
        auto encoded=encode_packet(packet);
        if(!encoded) return {Outcome::InvalidState};
        const auto word=[](const Identity& id,int i) {
            return std::uint32_t(id[i])|std::uint32_t(id[i+1])<<8|
                   std::uint32_t(id[i+2])<<16|std::uint32_t(id[i+3])<<24;
        };
        const auto low=word(packet.destination.identity,0), high=word(packet.destination.identity,4);
        // The original callback uses OR between individual GUID word matches.
        if(low!=ram_.read32(base_+0x2918+profile_.globals_delta) && high!=ram_.read32(base_+0x291c+profile_.globals_delta) &&
           low!=0xffffffffU && high!=0xffffffffU) return {Outcome::ForeignDestination};
        const auto dst=std::uint32_t(packet.destination.port[0])|std::uint32_t(packet.destination.port[1])<<8;
        const auto src=std::uint32_t(packet.source.port[0])|std::uint32_t(packet.source.port[1])<<8;
        std::uint32_t socket=0;
        for(std::uint32_t i=0;i<nsockets;++i) {
            auto candidate=ram_.read32(array+4*i);
            if(!candidate) continue;
            if(!ram_.valid(candidate,0x54)) return {Outcome::InvalidState};
            auto remote=ram_.read32(candidate+4);
            if(ram_.read32(candidate+8)==dst && (std::int32_t(remote)<0 || remote==src)) {
                socket=candidate; break;
            }
        }
        if(!socket) return {Outcome::NoSocket};
        const auto write=ram_.read32(socket+0x44),read=ram_.read32(socket+0x48),capacity=ram_.read32(socket+0x4c);
        if(capacity!=8 || write>=capacity || read>=capacity) return {Outcome::InvalidState};
        const auto used=(write+capacity-read)%capacity;
        if(used+1>=capacity) return {Outcome::Full};
        const auto node=ram_.read32(base_+0x2920+profile_.globals_delta);
        if(!node) return {Outcome::NoBuffers};
        if(!ram_.valid(node,0x24)) return {Outcome::InvalidState};
        const auto next=ram_.read32(node),payload=ram_.read32(node+0x20);
        const auto length=std::min<std::size_t>(packet.payload.size(),maximum);
        if((next && !ram_.valid(next,8)) || !payload || !ram_.valid(payload,length)) return {Outcome::InvalidState};
        const auto event=ram_.read32(socket+0x50);
        if(used==0 && std::int32_t(event)<=0) return {Outcome::InvalidState};
        // All addresses and structural invariants checked before changing RAM.
        ram_.write32(base_+0x2920+profile_.globals_delta,next);
        if(next) ram_.write32(next+4,0);
        ram_.write32(node+8,static_cast<std::uint32_t>(length));
        ram_.write(node+0xc,std::span<const std::uint8_t>(*encoded).first(header_size));
        ram_.write(payload,std::span<const std::uint8_t>(packet.payload).first(length));
        ram_.write32(socket+0x24+4*write,node);
        ram_.write32(socket+0x44,(write+1)%capacity);
        return {Outcome::Queued,event,used==0};
    }
private:
    GuestRam& ram_;
    std::uint32_t base_;
    ModuleProfile profile_;
};
} // namespace tc3::cabinet
