#pragma once
#include "pcsx2/R5900.h"
#include <array>
#include <span>
#include <string_view>

namespace tc3::native
{
inline constexpr std::string_view kTst3ElfSha256 =
    "35c08b7a7a18b52963072d8bdbf00d5252ce5d80f817a8ca668872ba7e95986d";
inline constexpr u32 kBlockPc = 0x00153310;
inline constexpr std::array<u32, 7> kBlockWords{
    0x00041080, 0x00441021, 0x00021980, 0x3c020188,
    0x2442b970, 0x03e00008, 0x00431021};

// The caller supplies the digest it computed from the actual game ELF.
// This routine never validates a filename or trusts game/title identifiers.
// False leaves the complete canonical state untouched. The initial proof
// accepts only normal clock scaling and aligned returns outside delay slots.
[[nodiscard]] bool TryExecuteBlock(cpuRegisters &state,
    std::string_view verified_elf_sha256, std::span<const u32> words,
    int ee_cycle_rate = 0) noexcept;
}
