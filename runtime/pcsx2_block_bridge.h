#pragma once
#include <cstddef>
#include <cstdint>

// CPU-thread-only proof API. The host must pass the bytes of the image it
// actually loaded, after its normal ELF validation/loading. No filename,
// environment variable, game title or caller-supplied digest enables this.
// Until this loader integration exists, the optional provider stays dormant.
extern "C" bool tc3NativeBlockProofBindLoadedElf(const void *bytes, std::size_t size);
extern "C" void tc3NativeBlockProofClearBinding();
extern "C" std::uint32_t tc3TryNativeBlockProof();
