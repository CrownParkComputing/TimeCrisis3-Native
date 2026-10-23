#include "native_block_provider.h"
#include <algorithm>

namespace tc3::native
{
namespace
{
    constexpr u64 SignExtend(u32 value) noexcept
    {
        return (value & 0x80000000u) ? (u64(value) | 0xffffffff00000000ull) : u64(value);
    }
}

bool TryExecuteBlock(cpuRegisters &state, std::string_view verified_elf_sha256,
    std::span<const u32> words, int ee_cycle_rate) noexcept
{
    if (verified_elf_sha256 != kTst3ElfSha256 || state.pc != kBlockPc ||
        words.size() != kBlockWords.size() ||
        !std::equal(words.begin(), words.end(), kBlockWords.begin()) ||
        ee_cycle_rate != 0 || state.branch || state.IsDelaySlot ||
        (state.GPR.r[31].UL[0] & 3u))
        return false;

    // Original TST3 block 00153310..00153328. Every operation below is a
    // 32-bit EE operation, sign-extending the low64 bits and retaining high64.
    // The JR target is captured before its ADDU delay slot.
    const u32 argument = state.GPR.r[4].UL[0];
    const u32 shifted = argument << 2;
    const u32 times_five = shifted + argument;
    const u32 offset = times_five << 6;
    const u32 base = 0x01880000u + 0xffffb970u;
    const u32 return_pc = state.GPR.r[31].UL[0];
    state.GPR.r[3].UD[0] = SignExtend(offset);
    state.GPR.r[2].UD[0] = SignExtend(base + offset);
    state.pc = return_pc;

    // Seven Default=9 fixed-point opcode costs from the pinned EE backend.
    // COP0.Config.DIE doubles them when disabled; the unmodified dispatcher
    // performs the pending-event test after this bounded block returns.
    const u32 fixed_cycles = 7u * 9u * (2u - ((state.CP0.n.Config >> 18) & 1u));
    state.cycle += std::max(1u, fixed_cycles >> 3);
    return true;
}
}
