#pragma once
#include "Protocol.h"

// Routing decisions are independent of the network interface and can be tested without Npcap.
struct RouteEntry {
    IpAddress destination{};
    IpAddress mask{};
    IpAddress gateway{};
    int port = 0;
    bool viaGateway = false;
};

namespace RouterCore {
inline uint16_t Checksum(const unsigned char* data, size_t length) {
    uint32_t sum = 0;
    while (length >= 2) { sum += Wire::Read16(data); data += 2; length -= 2; }
    if (length) sum += static_cast<uint16_t>(*data) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

inline int PrefixLength(uint32_t mask) {
    int bits = 0;
    while (mask & 0x80000000u) { ++bits; mask <<= 1; }
    return mask == 0 ? bits : -1;
}

inline bool ValidRoute(const RouteEntry& r) {
    const uint32_t mask = Wire::Read32(r.mask.data());
    const uint32_t network = Wire::Read32(r.destination.data());
    return r.port >= 0 && r.port < 2 && PrefixLength(mask) >= 0 &&
        (network & mask) == network && (!r.viaGateway ||
        (r.gateway[0] != 0 && r.gateway[0] < 224));
}

inline bool Select(const std::vector<RouteEntry>& routes, const IpAddress& destination, RouteEntry& result) {
    int best = -1;
    const uint32_t ip = Wire::Read32(destination.data());
    for (const auto& route : routes) {
        if (!ValidRoute(route)) continue;
        const uint32_t mask = Wire::Read32(route.mask.data());
        const int prefix = PrefixLength(mask);
        if (prefix > best && (ip & mask) == Wire::Read32(route.destination.data())) {
            best = prefix;
            result = route;
        }
    }
    return best >= 0;
}

// Reject malformed packets before changing the TTL and preserve IPv4 options and fragments.
inline bool Prepare(const unsigned char* data, size_t length, std::vector<unsigned char>& packet) {
    if (!data || length < 20 || (data[0] >> 4) != 4) return false;
    const size_t header = (data[0] & 15) * 4;
    const size_t total = Wire::Read16(data + 2);
    if (header < 20 || header > length || total < header || total > length ||
        total > Wire::MTU || data[8] <= 1 || Checksum(data, header) != 0) return false;
    packet.assign(data, data + total);
    --packet[8];
    packet[10] = packet[11] = 0;
    Wire::Write16(packet.data() + 10, Checksum(packet.data(), header));
    return true;
}
}
