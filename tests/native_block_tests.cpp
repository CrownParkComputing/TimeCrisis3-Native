#include "native_block_provider.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>

namespace R5900::Interpreter::OpcodeImpl
{
void SLL(); void ADDU(); void LUI(); void ADDIU();
}
alignas(16) cpuRegistersPack _cpuRegistersPack{};

static void require(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

// Real original scalar handlers compiled from R5900OpcodeImpl.cpp unchanged.
// This bounded harness supplies instruction fetch and JR/delay sequencing;
// it does not claim to validate the full VM event scheduler or BIOS boot.
static void original_block()
{
    const u32 saved_code = cpuRegs.code;
    const u32 return_pc = cpuRegs.GPR.r[31].UL[0];
    using namespace R5900::Interpreter::OpcodeImpl;
    void (*handlers[])() = {SLL, ADDU, SLL, LUI, ADDIU, nullptr, ADDU};
    for (unsigned i = 0; i < tc3::native::kBlockWords.size(); ++i)
    {
        cpuRegs.code = tc3::native::kBlockWords[i];
        cpuRegs.pc = tc3::native::kBlockPc + (i + 1) * 4;
        if (handlers[i]) handlers[i]();
    }
    cpuRegs.pc = return_pc;
    cpuRegs.code = saved_code; // interpreter decoding scratch, not block ABI
    const u32 fixed_cycles = 63u * (2u - ((cpuRegs.CP0.n.Config >> 18) & 1u));
    cpuRegs.cycle += fixed_cycles >> 3;
}

int main()
{
    std::mt19937_64 random(0x544333);
    unsigned comparisons = 0;
    for (unsigned i = 0; i < 10000; ++i)
    {
        cpuRegisters initial{};
        for (auto &r : initial.GPR.r) { r.UD[0] = random(); r.UD[1] = random(); }
        initial.GPR.r[0] = {};
        initial.GPR.r[31].UL[0] &= ~3u;
        initial.pc = tc3::native::kBlockPc;
        initial.code = random(); initial.cycle = random();
        initial.CP0.n.Config = random(); initial.nextEventCycle = random();
        initial.HI.UD[0] = random(); initial.LO.UD[1] = random();
        if (i < 8) initial.GPR.r[4].UD[0] = std::array<u64,8>{0,1,0x7fffffff,0x80000000,0xffffffff,0x12345678,0x01000000,0xffffffffffffffffull}[i];
        cpuRegs = initial; original_block();
        cpuRegisters actual = initial;
        require(tc3::native::TryExecuteBlock(actual, tc3::native::kTst3ElfSha256,
            tc3::native::kBlockWords), "verified block rejected");
        require(std::memcmp(&actual, &cpuRegs, sizeof(actual)) == 0,
            "canonical state differs from original scalar handlers");
        ++comparisons;
    }
    cpuRegisters state{}; state.pc = tc3::native::kBlockPc;
    auto reject = [&](std::string_view hash, std::span<const u32> words, int rate=0) {
        const auto before = state;
        require(!tc3::native::TryExecuteBlock(state, hash, words, rate), "unsupported block accepted");
        require(!std::memcmp(&before, &state, sizeof(state)), "rejection changed state");
    };
    reject("wrong image", tc3::native::kBlockWords);
    for (unsigned i=0; i<7; ++i) { auto words=tc3::native::kBlockWords; words[i]^=1; reject(tc3::native::kTst3ElfSha256, words); }
    reject(tc3::native::kTst3ElfSha256, std::span(tc3::native::kBlockWords).first(6));
    reject(tc3::native::kTst3ElfSha256, tc3::native::kBlockWords, 1);
    state.pc += 4; reject(tc3::native::kTst3ElfSha256, tc3::native::kBlockWords); state.pc -= 4;
    state.branch=1; reject(tc3::native::kTst3ElfSha256, tc3::native::kBlockWords); state.branch=0;
    state.IsDelaySlot=1; reject(tc3::native::kTst3ElfSha256, tc3::native::kBlockWords); state.IsDelaySlot=0;
    state.GPR.r[31].UL[0]=1; reject(tc3::native::kTst3ElfSha256, tc3::native::kBlockWords);
    std::cout << "PASS: " << comparisons << " canonical-state comparisons against original PCSX2 scalar handlers; image/byte/PC/mode rejection\n";
}
