#include "pcsx2/Common.h"
#include "pcsx2/Memory.h"
#include "3rdparty/lzma/include/Sha256.h"
#include "native_block_provider.h"
#include "pcsx2_block_bridge.h"

namespace
{
bool bound_to_loaded_tst3 = false;
}

extern "C" bool tc3NativeBlockProofBindLoadedElf(const void *bytes, std::size_t size)
{
    bound_to_loaded_tst3 = false;
    if (!bytes || !size) return false;
    CSha256 hash;
    Sha256_Init(&hash);
    if (!Sha256_SetFunction(&hash, SHA256_ALGO_SW)) return false;
    Sha256_Update(&hash, static_cast<const Byte *>(bytes), size);
    Byte digest[SHA256_DIGEST_SIZE];
    Sha256_Final(&hash, digest);
    char hex[SHA256_DIGEST_SIZE * 2];
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i=0; i<SHA256_DIGEST_SIZE; ++i)
    {
        hex[i*2] = digits[digest[i] >> 4];
        hex[i*2+1] = digits[digest[i] & 15];
    }
    bound_to_loaded_tst3 = std::string_view(hex, sizeof(hex)) == tc3::native::kTst3ElfSha256;
    return bound_to_loaded_tst3;
}

extern "C" void tc3NativeBlockProofClearBinding()
{
    bound_to_loaded_tst3 = false;
}

extern "C" std::uint32_t tc3TryNativeBlockProof()
{
    if (!bound_to_loaded_tst3 || cpuRegs.pc != tc3::native::kBlockPc ||
        EmuConfig.Gamefixes.GoemonTlbHack)
        return 0;
    std::array<u32, tc3::native::kBlockWords.size()> words;
    for (std::size_t i=0; i<words.size(); ++i)
    {
        const u32 address = cpuRegs.pc + static_cast<u32>(i)*4;
        // Existing JIT compilation owns debugger/exception handling.
        if (isBreakpointNeeded(address) || isMemcheckNeeded(address)) return 0;
        words[i] = memRead32(address);
    }
    return tc3::native::TryExecuteBlock(cpuRegs, tc3::native::kTst3ElfSha256,
        words, EmuConfig.Speedhacks.EECycleRate) ? 1u : 0u;
}
