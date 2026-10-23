// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tc3::cabinet {
using Identity = std::array<std::uint8_t, 8>;
// Preserve network-order port bytes exactly as the original ILSOCK does.
using Port = std::array<std::uint8_t, 2>;
struct Address {
    Port port{};
    Identity identity{};
    bool operator==(const Address&) const = default;
};
struct Packet {
    Address destination;
    Address source;
    std::vector<std::uint8_t> payload;
    bool operator==(const Packet&) const = default;
};
inline constexpr std::size_t header_size = 20;
inline constexpr std::size_t default_max_payload = 492;

// Original ILSOCK RVA 0x1664 accepts addresses of at least 12 bytes, family 60.
inline std::optional<Address> decode_address(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || bytes[1] != 0x3c) return std::nullopt;
    Address result;
    std::copy_n(bytes.begin()+2, 2, result.port.begin());
    std::copy_n(bytes.begin()+4, 8, result.identity.begin());
    return result;
}
inline std::array<std::uint8_t, 12> encode_address(const Address& address) {
    std::array<std::uint8_t, 12> bytes{12, 0x3c};
    std::copy(address.port.begin(), address.port.end(), bytes.begin()+2);
    std::copy(address.identity.begin(), address.identity.end(), bytes.begin()+4);
    return bytes;
}
// Original ILSOCK RVAs 0x1994 and 0x1bc0 prepend these 20 bytes before the
// opaque game data. Identity/port bytes already have their guest wire order.
inline std::optional<std::vector<std::uint8_t>> encode_packet(
    const Packet& packet, std::size_t max_payload=default_max_payload) {
    if (packet.payload.size() > max_payload) return std::nullopt;
    std::vector<std::uint8_t> bytes(header_size+packet.payload.size());
    std::copy(packet.destination.identity.begin(), packet.destination.identity.end(), bytes.begin());
    std::copy(packet.destination.port.begin(), packet.destination.port.end(), bytes.begin()+8);
    std::copy(packet.source.port.begin(), packet.source.port.end(), bytes.begin()+10);
    std::copy(packet.source.identity.begin(), packet.source.identity.end(), bytes.begin()+12);
    std::copy(packet.payload.begin(), packet.payload.end(), bytes.begin()+20);
    return bytes;
}
inline std::optional<Packet> decode_packet(std::span<const std::uint8_t> bytes,
                                         std::size_t max_payload=default_max_payload) {
    if (bytes.size() < header_size || bytes.size()-header_size > max_payload) return std::nullopt;
    Packet packet;
    std::copy_n(bytes.begin(), 8, packet.destination.identity.begin());
    std::copy_n(bytes.begin()+8, 2, packet.destination.port.begin());
    std::copy_n(bytes.begin()+10, 2, packet.source.port.begin());
    std::copy_n(bytes.begin()+12, 8, packet.source.identity.begin());
    packet.payload.assign(bytes.begin()+20, bytes.end());
    return packet;
}
} // namespace tc3::cabinet
