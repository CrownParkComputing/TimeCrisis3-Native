// SPDX-License-Identifier: GPL-3.0-or-later
// Cabinet service options with direct EEPROM (ACSRAM) read/write.
// TC4 (NM00032, "TSF100-2-NA"): exact CRC16 checksum decoded from TC4LOAD
// 0x00344f60. TC3 (NM00012, "TST-03A "): 16-bit additive checksum at 0x190
// verified against captured cabinet SRAM.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace tc3::service {

struct Option {
    const char* label;
    std::uint8_t page;       // main-menu row of the page
    std::uint8_t row;        // row inside the page
    std::int16_t sram_off;   // byte offset in ACSRAM primary block
    std::uint8_t base;       // sram byte = base + index (when bit_mask == 0)
    const char* const* values;
    std::uint8_t num_values;
    std::uint32_t bit_mask;  // non-zero: bit inside u32 at 0x610 (0 = off, 1 = on)
};

inline const char* const kOnOff[] = {"Off", "On"};
inline const char* const kNoYes[] = {"No", "Yes"};
inline const char* const kLife[] = {"1", "2", "3", "4", "5"};
inline const char* const kDifficulty[] = {"A (Easiest)", "B (Easy)", "C (Medium)",
                                          "D (Hard)", "E (Very Hard)"};
inline const char* const kHitColor[] = {"Red", "Green"};
inline const char* const kCoins[] = {"1 Coin", "2 Coins", "3 Coins", "4 Coins", "5 Coins",
                                     "6 Coins", "7 Coins", "8 Coins", "9 Coins"};
inline const char* const kAttractCut[] = {"Off (Sound On)", "On (Muted)"};

// Offsets and bitfields verified from TC4LOAD (0x00332050..0x0033da90):
//   0x030: Game Cost (1..9)
//   0x031: Continue Cost (1..9, clamped <= Game Cost)
//   0x032: Player's Life (1..5)
//   0x033: Difficulty (0..4)
//   0x610 bit 0x002: Free Play (0=No, 1=Yes)
//   0x610 bit 0x004: Hit Color (0=Red, 1=Green)
//   0x610 bit 0x100: Attract Cut (0=Off, 1=On)
inline constexpr Option kOptions[] = {
    {"Player's life",     1, 2, 0x032, 1, kLife,       5, 0x000u},
    {"Difficulty",        1, 3, 0x033, 0, kDifficulty, 5, 0x000u},
    {"Hit color",         1, 4, 0x610, 0, kHitColor,   2, 0x004u},
    {"Free play",         0, 2, 0x610, 0, kNoYes,      2, 0x002u},
    {"Game cost",         0, 0, 0x030, 1, kCoins,      9, 0x000u},
    {"Continue cost",     0, 1, 0x031, 1, kCoins,      9, 0x000u},
    {"Attract sound cut", 6, 1, 0x610, 0, kAttractCut, 2, 0x100u},
};
inline constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

// Exact 256-entry CRC16 table from TC4LOAD @ VA 0x005ab3d0.
inline constexpr std::uint16_t kTc4CrcTable[256] = {
    0x5555u, 0x1727u, 0x2e4eu, 0x4575u, 0x5c9cu, 0x73c3u, 0x8aeau, 0xa211u,
    0xb938u, 0xd05fu, 0xe786u, 0xfeadu, 0x15d4u, 0x2cfbu, 0x4422u, 0x5b49u,
    0x8997u, 0x7270u, 0xb7e5u, 0xa0beu, 0xe633u, 0xcf0cu, 0x1481u, 0xfd5au,
    0x42cfu, 0x2ba8u, 0x711du, 0x59f6u, 0x9f6bu, 0x8844u, 0xcdb9u, 0xb692u,
    0x2a55u, 0x132eu, 0xfc07u, 0xe4e0u, 0x86f1u, 0x6fcau, 0x58a3u, 0x417cu,
    0xe38du, 0xcc66u, 0xb53fu, 0x9e18u, 0x4029u, 0x2902u, 0x11dbu, 0xfab4u,
    0x5750u, 0xf961u, 0xe23au, 0x9cc5u, 0x859eu, 0x6e77u, 0x3ed6u, 0x27afu,
    0x1088u, 0xcb13u, 0xb3ecu, 0x55fdu, 0xb299u, 0x9b72u, 0x844bu, 0x6d24u,
    0xf80eu, 0xe0e7u, 0xb146u, 0x9a1fu, 0xc9c0u, 0x6bd1u, 0x54aau, 0x0f35u,
    0x2509u, 0x0de2u, 0xf6bbu, 0xdf94u, 0x82f8u, 0x3d83u, 0x265cu, 0xc86du,
    0xde41u, 0xc71au, 0x81a5u, 0x9779u, 0x8052u, 0x6a7eu, 0x5357u, 0x23b6u,
    0x0c8fu, 0x3c30u, 0x692bu, 0x5204u, 0xf568u, 0xaff3u, 0x98ccu, 0x3addu,
    0xc5c7u, 0x9626u, 0x7effu, 0xaea0u, 0xdb9bu, 0xc474u, 0x50b1u, 0x398au,
    0xf415u, 0x09e9u, 0xf2c2u, 0xdceeu, 0x67d8u, 0x2263u, 0x0b3cu, 0xad4du,
    0xabfau, 0x6685u, 0x7c59u, 0x6532u, 0x4f5eu, 0xda48u, 0x94d3u, 0x3837u,
    0x0896u, 0xf16fu, 0x2110u, 0x4e0bu, 0x36e4u, 0xc321u, 0x7dacu, 0x1fbdu,
    0x9380u, 0xaaa7u, 0xc1ceu, 0xd8f5u, 0xf01cu, 0x0743u, 0x1e6au, 0x3591u,
    0x4cb8u, 0x63dfu, 0x7b06u, 0x922du, 0xa954u, 0xc07bu, 0xd7a2u, 0xeec9u,
    0x1d17u, 0x05f0u, 0x4b65u, 0x343eu, 0x79b3u, 0x628cu, 0xa801u, 0x90dau,
    0xd64fu, 0xbf28u, 0x049du, 0xed76u, 0x32ebu, 0x1bc4u, 0x6139u, 0x4a12u,
    0xbdd5u, 0xa6aeu, 0x8f87u, 0x7860u, 0x1a71u, 0x034au, 0xec23u, 0xd4fcu,
    0x770du, 0x5fe6u, 0x48bfu, 0x3198u, 0xd3a9u, 0xbc82u, 0xa55bu, 0x8e34u,
    0xead0u, 0x8ce1u, 0x75bau, 0x3045u, 0x191eu, 0x01f7u, 0xd256u, 0xbb2fu,
    0xa408u, 0x5e93u, 0x476cu, 0xe97du, 0x4619u, 0x2ef2u, 0x17cbu, 0x00a4u,
    0x8b8eu, 0x7467u, 0x44c6u, 0x2d9fu, 0x5d40u, 0xff51u, 0xe82au, 0xa2b5u,
    0xb889u, 0xa162u, 0x8a3bu, 0x7314u, 0x1678u, 0xd103u, 0xb9dcu, 0x5bedu,
    0x71c1u, 0x5a9au, 0x1525u, 0x2af9u, 0x13d2u, 0xfdfeu, 0xe6d7u, 0xb736u,
    0xa00fu, 0xcfb0u, 0xfcabu, 0xe584u, 0x88e8u, 0x4373u, 0x2c4cu, 0xce5du,
    0x5947u, 0x29a6u, 0x127fu, 0x4220u, 0x6f1bu, 0x57f4u, 0xe431u, 0xcd0au,
    0x8795u, 0x9d69u, 0x8642u, 0x706eu, 0xfb58u, 0xb5e3u, 0x9ebcu, 0x40cdu,
    0x3f7au, 0xfa05u, 0x0fd9u, 0xf8b2u, 0xe2deu, 0x6dc8u, 0x2853u, 0xcbb7u,
    0x9c16u, 0x84efu, 0xb490u, 0xe18bu, 0xca64u, 0x56a1u, 0x112cu, 0xb33du,
};

inline std::uint16_t compute_crc16(const std::uint8_t* sram) {
    std::uint16_t crc = 0;
    for (int i = 0; i < 0x614; ++i)
        crc = static_cast<std::uint16_t>(((crc << 8) & 0xffffu) ^
                                         kTc4CrcTable[((crc >> 8) ^ sram[i]) & 0xffu]);
    return crc;
}

inline bool is_tc4_sram(const std::uint8_t* sram) {
    return std::memcmp(sram, "TSF100-2-NA", 11) == 0;
}

inline bool is_tc3_sram(const std::uint8_t* sram) {
    return std::memcmp(sram + 4, "TST-03A ", 8) == 0;
}

inline std::uint16_t compute_tc3_checksum(const std::uint8_t* sram) {
    std::uint16_t sum = 0;
    for (int i = 0; i < 0x190; ++i)
        sum = static_cast<std::uint16_t>(sum + sram[i]);
    return sum;
}

inline int read_option(const std::uint8_t* sram, int opt_idx) {
    if (opt_idx < 0 || opt_idx >= kOptionCount) return -1;
    const auto& o = kOptions[opt_idx];
    if (is_tc3_sram(sram)) {
        // TC3 ("TST-03A ") stores each option as a 16-bit LE word at 0x00c..0x024:
        //   0: Player's life     @ 0x012 (1..5)
        //   1: Difficulty        @ 0x014 (0..4)
        //   2: Hit color         @ 0x016 (0=Red, 1=Green)
        //   3: Free play         @ 0x010 (0=No, 1=Yes)
        //   4: Game cost         @ 0x00c (1..9)
        //   5: Continue cost     @ 0x00e (1..9)
        //   6: Attract sound cut @ 0x024 (1=Sound On -> 0, 0=Muted -> 1)
        static constexpr int kTc3Off[kOptionCount] = {0x012, 0x014, 0x016, 0x010, 0x00c, 0x00e, 0x024};
        const int v = sram[kTc3Off[opt_idx]];
        const int idx = (opt_idx == 6) ? (v == 0 ? 1 : 0) : (v - int(o.base));
        return (idx >= 0 && idx < o.num_values) ? idx : -1;
    }
    if (o.bit_mask != 0) {
        std::uint32_t word = 0;
        std::memcpy(&word, sram + 0x610, 4);
        return (word & o.bit_mask) ? 1 : 0;
    }
    const int idx = int(sram[o.sram_off]) - int(o.base);
    return (idx >= 0 && idx < o.num_values) ? idx : -1;
}

// Apply target values (-1 = keep existing), recompute the game-specific
// EEPROM checksum, and mirror the primary block so both TC3 ("TST-03A ") and
// TC4 ("TSF100-2-NA") accept the edited EEPROM on boot.
inline void write_options_and_commit(std::uint8_t* sram, const int* targets, int count) {
    if (is_tc3_sram(sram)) {
        static constexpr int kTc3Off[kOptionCount] = {0x012, 0x014, 0x016, 0x010, 0x00c, 0x00e, 0x024};
        for (int i = 0; i < kOptionCount && i < count; ++i) {
            const int t = targets[i];
            if (t < 0 || t >= kOptions[i].num_values) continue;
            const int off = kTc3Off[i];
            const std::uint8_t val = (i == 6) ? static_cast<std::uint8_t>(t == 0 ? 1 : 0)
                                              : static_cast<std::uint8_t>(kOptions[i].base + t);
            sram[off] = val;
            sram[off + 1] = 0;
        }
        if (sram[0x00c] >= 1 && sram[0x00e] > sram[0x00c])
            sram[0x00e] = sram[0x00c];
        const std::uint16_t ck = compute_tc3_checksum(sram);
        sram[0x190] = static_cast<std::uint8_t>(ck & 0xffu);
        sram[0x191] = static_cast<std::uint8_t>((ck >> 8) & 0xffu);
        // Mirror block at 0x194 holds sram[4..0x192) (starts with "TST-03A ",
        // ends with the checksum) per captured NM00012 cabinet SRAM.
        std::memcpy(sram + 0x194, sram + 4, 0x18e);
        return;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, sram + 0x610, 4);
    bool changed = false;
    for (int i = 0; i < kOptionCount && i < count; ++i) {
        const int t = targets[i];
        if (t < 0 || t >= kOptions[i].num_values) continue;
        const auto& o = kOptions[i];
        if (o.bit_mask != 0) {
            if (t) bits |= o.bit_mask;
            else bits &= ~o.bit_mask;
        } else {
            sram[o.sram_off] = static_cast<std::uint8_t>(o.base + t);
        }
        changed = true;
    }
    // Keep Continue Cost <= Game Cost as enforced by TC4's COIN OPTIONS editor.
    if (sram[0x030] >= 1 && sram[0x031] > sram[0x030])
        sram[0x031] = sram[0x030];
    if (changed)
        bits |= 0x08000000u; // non-default flag set by TC4's test menu on any edit
    std::memcpy(sram + 0x610, &bits, 4);
    const std::uint16_t crc = compute_crc16(sram);
    sram[0x614] = static_cast<std::uint8_t>(crc & 0xffu);
    sram[0x615] = static_cast<std::uint8_t>((crc >> 8) & 0xffu);
    std::memcpy(sram + 0x640, sram, 0x616);
}

enum class ApplyState { Idle, Working, Done, Failed };

} // namespace tc3::service
