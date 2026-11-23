#pragma once
#include "RouterCore.h"

// Called under the router lock. Times are monotonic milliseconds.
namespace NatCore {
constexpr unsigned char Icmp = 1, Tcp = 6, Udp = 17;
constexpr int Inside = 0, Outside = 1;
constexpr uint64_t ShortIdleMs = 60000, TcpIdleMs = 600000;

struct Rule {
    unsigned char protocol;
    uint16_t outside;
    IpAddress insideIp;
    uint16_t inside;
};

struct Mapping {
    unsigned char protocol;
    IpAddress insideIp;
    uint16_t inside;
    uint16_t outside;
    IpAddress remoteIp;
    uint16_t remotePort;
    bool forwarded;
    uint64_t expires;
};

inline IpAddress Address(const unsigned char* bytes) {
    return {bytes[0], bytes[1], bytes[2], bytes[3]};
}

inline bool Unicast(const IpAddress& ip) {
    return ip[0] != 0 && ip[0] != 127 && ip[0] < 224;
}

inline uint16_t TransportChecksum(const unsigned char* ip, size_t header, size_t length) {
    if (ip[9] == Icmp) return RouterCore::Checksum(ip + header, length);

    // TCP and UDP include the IPv4 pseudo-header in the one's-complement sum.
    uint32_t sum = ip[9] + static_cast<uint32_t>(length);
    for (int offset = 12; offset < 20; offset += 2) sum += Wire::Read16(ip + offset);
    sum += static_cast<uint16_t>(~RouterCore::Checksum(ip + header, length));
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

struct Transport {
    size_t header = 0;
    size_t length = 0;
    size_t checksumOffset = 0;
    uint16_t source = 0, destination = 0;
    bool noChecksum = false;
    bool openingSyn = false;
};

// The IPv4 header and total length have already been checked by RouterCore::Prepare.
inline bool Parse(const std::vector<unsigned char>& packet, bool inbound, Transport& result) {
    if (Wire::Read16(packet.data() + 6) & 0x3fff) return false;
    const auto protocol = packet[9];
    result.header = (packet[0] & 15) * 4;
    result.length = packet.size() - result.header;
    const auto* payload = packet.data() + result.header;
    if (protocol == Icmp) {
        if (result.length < 8 || payload[0] != (inbound ? 0 : 8) || payload[1] != 0) return false;
        result.source = result.destination = Wire::Read16(payload + 4);
        result.checksumOffset = 2;
    } else if (protocol == Tcp || protocol == Udp) {
        if (protocol == Tcp) {
            if (result.length < 20) return false;
            const size_t tcpHeader = (payload[12] >> 4) * 4;
            if (tcpHeader < 20 || tcpHeader > result.length) return false;
            result.checksumOffset = 16;
            result.openingSyn = (payload[13] & 0x17) == 0x02;
        } else {
            if (result.length < 8 || Wire::Read16(payload + 4) != result.length) return false;
            result.checksumOffset = 6;
            result.noChecksum = Wire::Read16(payload + 6) == 0;
        }
        result.source = Wire::Read16(payload);
        result.destination = Wire::Read16(payload + 2);
        if (!result.source || !result.destination) return false;
    } else {
        return false;
    }
    return result.noChecksum || TransportChecksum(packet.data(), result.header, result.length) == 0;
}

inline void Rewrite(std::vector<unsigned char>& packet, const Transport& transport,
                    bool inbound, const IpAddress& address, uint16_t value) {
    std::copy(address.begin(), address.end(), packet.begin() + (inbound ? 16 : 12));
    auto* payload = packet.data() + transport.header;
    Wire::Write16(payload + (packet[9] == Icmp ? 4 : inbound ? 2 : 0), value);
    if (!transport.noChecksum) {
        Wire::Write16(payload + transport.checksumOffset, 0);
        const auto checksum = TransportChecksum(packet.data(), transport.header, transport.length);
        Wire::Write16(payload + transport.checksumOffset, packet[9] == Udp && checksum == 0 ? 0xffff : checksum);
    }
    Wire::Write16(packet.data() + 10, 0);
    Wire::Write16(packet.data() + 10, RouterCore::Checksum(packet.data(), transport.header));
}

class Table {
public:
    bool AddRule(const Rule& rule) {
        if ((rule.protocol != Tcp && rule.protocol != Udp) || !rule.outside || !rule.inside ||
            !Unicast(rule.insideIp) || rules_.size() >= 64) return false;
        if (Reserved(rule.protocol, rule.outside) ||
            std::any_of(rules_.begin(), rules_.end(), [&](const Rule& other) {
                return other.protocol == rule.protocol && other.insideIp == rule.insideIp &&
                       other.inside == rule.inside;
            })) return false;
        if (std::any_of(mappings_.begin(), mappings_.end(), [&](const Mapping& row) {
                return row.protocol == rule.protocol && row.outside == rule.outside;
            })) return false;
        rules_.push_back(rule);
        return true;
    }

    void DeleteRule(unsigned char protocol, uint16_t outside) {
        rules_.erase(std::remove_if(rules_.begin(), rules_.end(), [&](const Rule& rule) {
            return rule.protocol == protocol && rule.outside == outside;
        }), rules_.end());
        mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [&](const Mapping& row) {
            return row.forwarded && row.protocol == protocol && row.outside == outside;
        }), mappings_.end());
    }

    const std::vector<Mapping>& Mappings() const { return mappings_; }
    void ClearMappings() { mappings_.clear(); }
    void Expire(uint64_t now) {
        mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [&](const Mapping& row) {
            return now >= row.expires;
        }), mappings_.end());
    }

    // At this point the packet has passed IPv4 validation and TTL was already decremented.
    bool Forward(std::vector<unsigned char>& packet, int ingress, const IpAddress& insideIp,
                 const IpAddress& outsideIp, const std::vector<RouteEntry>& routes,
                 uint64_t now, RouteEntry& route, IpAddress& destination) {
        destination = Address(packet.data() + 16);
        if (ingress == Outside && destination == outsideIp) {
            Expire(now);
            return Inbound(packet, insideIp, outsideIp, routes, now, route, destination);
        }
        if (!Unicast(destination) || destination == insideIp || destination == outsideIp ||
            !RouterCore::Select(routes, destination, route)) return false;
        if (ingress == Outside) return route.port == Outside;
        if (route.port == Inside) return true;
        Expire(now);
        return Outbound(packet, outsideIp, now);
    }

private:
    static constexpr size_t MaximumMappings = 1024;
    static constexpr uint16_t FirstDynamic = 49152, LastDynamic = 65535;
    std::vector<Mapping> mappings_;
    std::vector<Rule> rules_;
    uint16_t next_ = 50001;

    const Rule* Reserved(unsigned char protocol, uint16_t outside) const {
        const auto it = std::find_if(rules_.begin(), rules_.end(), [&](const Rule& rule) {
            return rule.protocol == protocol && rule.outside == outside;
        });
        return it == rules_.end() ? nullptr : &*it;
    }

    uint16_t Allocate(unsigned char protocol) {
        for (unsigned i = FirstDynamic; i <= LastDynamic; ++i) {
            const uint16_t candidate = next_;
            next_ = next_ == LastDynamic ? FirstDynamic : static_cast<uint16_t>(next_ + 1);
            if (Reserved(protocol, candidate)) continue;
            if (std::none_of(mappings_.begin(), mappings_.end(), [&](const Mapping& row) {
                    return row.protocol == protocol && row.outside == candidate;
                })) return candidate;
        }
        return 0;
    }

    static uint64_t Deadline(unsigned char protocol, uint64_t now) {
        return now + (protocol == Tcp ? TcpIdleMs : ShortIdleMs);
    }

    bool Outbound(std::vector<unsigned char>& packet, const IpAddress& outsideIp, uint64_t now) {
        Transport transport;
        if (!Parse(packet, false, transport)) return false;
        const auto protocol = packet[9];
        const auto source = Address(packet.data() + 12);
        const auto remote = Address(packet.data() + 16);
        if (!Unicast(source) || !Unicast(outsideIp)) return false;
        auto row = std::find_if(mappings_.begin(), mappings_.end(), [&](const Mapping& item) {
            return item.protocol == protocol && item.insideIp == source && item.inside == transport.source &&
                   (protocol == Icmp || (item.remoteIp == remote && item.remotePort == transport.destination));
        });
        if (row == mappings_.end()) {
            if (mappings_.size() >= MaximumMappings) return false;
            const auto outside = Allocate(protocol);
            if (!outside) return false;
            mappings_.push_back({protocol, source, transport.source, outside, remote, transport.destination,
                                 false, Deadline(protocol, now)});
            row = mappings_.end() - 1;
        }
        row->expires = Deadline(protocol, now);
        Rewrite(packet, transport, false, outsideIp, row->outside);
        return true;
    }

    bool Inbound(std::vector<unsigned char>& packet, const IpAddress& insideIp,
                 const IpAddress& outsideIp, const std::vector<RouteEntry>& routes, uint64_t now,
                 RouteEntry& route, IpAddress& destination) {
        Transport transport;
        const auto remote = Address(packet.data() + 12);
        if (!Unicast(remote) || !Parse(packet, true, transport)) return false;
        const auto protocol = packet[9];
        auto row = std::find_if(mappings_.begin(), mappings_.end(), [&](const Mapping& item) {
            return item.protocol == protocol && item.outside == transport.destination &&
                   (protocol == Icmp || (item.remoteIp == remote && item.remotePort == transport.source));
        });
        const Rule* rule = nullptr;
        if (row == mappings_.end()) {
            rule = Reserved(protocol, transport.destination);
            if (!rule || mappings_.size() >= MaximumMappings ||
                (protocol == Tcp && !transport.openingSyn)) return false;
            // One inside/remote TCP or UDP tuple must have one unambiguous return mapping.
            if (std::any_of(mappings_.begin(), mappings_.end(), [&](const Mapping& item) {
                    return item.protocol == protocol && item.insideIp == rule->insideIp &&
                           item.inside == rule->inside && item.remoteIp == remote &&
                           item.remotePort == transport.source;
                })) return false;
        }
        destination = rule ? rule->insideIp : row->insideIp;
        if (destination == insideIp || destination == outsideIp ||
            !RouterCore::Select(routes, destination, route) || route.port != Inside) return false;
        if (rule) {
            mappings_.push_back({protocol, rule->insideIp, rule->inside, rule->outside, remote, transport.source,
                                 true, Deadline(protocol, now)});
            row = mappings_.end() - 1;
        }
        row->expires = Deadline(protocol, now);
        Rewrite(packet, transport, true, destination, row->inside);
        return true;
    }
};
}
