#include "pch.h"
#include "ARPLayer.h"

namespace {
// ARP 메시지(IP over Ethernet) 28바이트 구성
constexpr size_t kArpMessageSize = 28;
constexpr uint16_t kHardwareEthernet = 1;
constexpr unsigned char kHardwareSize = 6;
constexpr unsigned char kProtocolSize = 4;

constexpr size_t kHardwareTypeOffset = 0;
constexpr size_t kProtocolTypeOffset = 2;
constexpr size_t kHardwareSizeOffset = 4;
constexpr size_t kProtocolSizeOffset = 5;
constexpr size_t kOperationOffset = 6;
constexpr size_t kSenderMacOffset = 8;
constexpr size_t kSenderIpOffset = 14;
constexpr size_t kTargetMacOffset = 18;
constexpr size_t kTargetIpOffset = 24;

constexpr IpAddress kUnspecifiedIp{};

std::wstring MappingText(const IpAddress& ip, const MacAddress& mac) {
    return Wire::IpText(ip) + L" → " + Wire::MacText(mac);
}
}  // namespace

bool CARPLayer::SendArp(
    uint16_t operation,
    const MacAddress& ethernetDestination,
    const MacAddress& targetMac,
    const IpAddress& targetIp) {
    auto* ethernetLayer = static_cast<CEthernetLayer*>(GetUnderLayer());
    if (ethernetLayer == nullptr) {
        return false;
    }

    MacAddress senderMac{};
    IpAddress senderIp{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        senderMac = mac_;
        senderIp = ip_;
    }

    std::array<unsigned char, kArpMessageSize> message{};
    Wire::Write16(message.data() + kHardwareTypeOffset, kHardwareEthernet);
    Wire::Write16(message.data() + kProtocolTypeOffset, Wire::IpType);
    message[kHardwareSizeOffset] = kHardwareSize;
    message[kProtocolSizeOffset] = kProtocolSize;
    Wire::Write16(message.data() + kOperationOffset, operation);
    std::copy(senderMac.begin(), senderMac.end(), message.begin() + kSenderMacOffset);
    std::copy(senderIp.begin(), senderIp.end(), message.begin() + kSenderIpOffset);
    std::copy(targetMac.begin(), targetMac.end(), message.begin() + kTargetMacOffset);
    std::copy(targetIp.begin(), targetIp.end(), message.begin() + kTargetIpOffset);

    return ethernetLayer->SendPacket(
        message.data(),
        message.size(),
        Wire::ArpType,
        ethernetDestination);
}

bool CARPLayer::SendRequest(const IpAddress& target) {
    if (target == kUnspecifiedIp) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (target == ip_) {
            return false;
        }

        // 아직 응답을 받지 못한 주소는 Incomplete 상태로 3분간 유지한다.
        // 이미 Complete인 항목은 응답이 올 때까지 기존 매핑을 그대로 사용한다.
        auto found = cache_.find(target);
        if (found == cache_.end() || found->second.state == EntryState::Incomplete) {
            CacheEntry& entry = cache_[target];
            entry.ip = target;
            entry.mac = {};
            entry.state = EntryState::Incomplete;
            entry.expires = GetTickCount64() + kIncompleteTimeoutMs;
        }
    }
    CacheChanged();

    // 목적지 Ethernet 주소는 브로드캐스트, 대상 하드웨어 주소는 아직 모르므로 0으로 채운다.
    const bool sent = SendArp(kRequest, Wire::BroadcastMac, MacAddress{}, target);
    Status(
        sent ? L"ARP 요청 송신: " + Wire::IpText(target) + L"의 MAC 주소는?"
             : L"ARP 요청 송신 실패");
    return sent;
}

bool CARPLayer::Lookup(const IpAddress& target, MacAddress& mac) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = cache_.find(target);
    if (found == cache_.end() || found->second.state != EntryState::Complete) {
        return false;
    }
    mac = found->second.mac;
    return true;
}

std::vector<CARPLayer::CacheEntry> CARPLayer::Snapshot() {
    std::vector<CacheEntry> entries;
    std::lock_guard<std::mutex> lock(mutex_);
    entries.reserve(cache_.size());
    for (const auto& item : cache_) {
        entries.push_back(item.second);
    }
    return entries;
}

bool CARPLayer::Remove(const IpAddress& target) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = cache_.erase(target) > 0;
    }
    if (removed) {
        CacheChanged();
    }
    return removed;
}

void CARPLayer::Clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }
    CacheChanged();
}

void CARPLayer::Expire(ULONGLONG now) {
    std::vector<IpAddress> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto item = cache_.begin(); item != cache_.end();) {
            if (now >= item->second.expires) {
                expired.push_back(item->first);
                item = cache_.erase(item);
            } else {
                ++item;
            }
        }
    }

    if (expired.empty()) {
        return;
    }

    for (const IpAddress& ip : expired) {
        Status(L"ARP 캐시 항목 만료: " + Wire::IpText(ip));
    }
    CacheChanged();
}

bool CARPLayer::ReceiveFrame(
    const unsigned char* data,
    size_t length,
    const FrameContext&) {
    // Ethernet 패딩이 붙어 올 수 있으므로 최소 길이만 검사한다.
    if (data == nullptr || length < kArpMessageSize) {
        return false;
    }

    const bool isIpOverEthernet =
        Wire::Read16(data + kHardwareTypeOffset) == kHardwareEthernet &&
        Wire::Read16(data + kProtocolTypeOffset) == Wire::IpType &&
        data[kHardwareSizeOffset] == kHardwareSize &&
        data[kProtocolSizeOffset] == kProtocolSize;
    const uint16_t operation = Wire::Read16(data + kOperationOffset);
    if (!isIpOverEthernet || (operation != kRequest && operation != kReply)) {
        return false;
    }

    MacAddress senderMac{};
    IpAddress senderIp{};
    IpAddress targetIp{};
    std::copy(data + kSenderMacOffset, data + kSenderMacOffset + 6, senderMac.begin());
    std::copy(data + kSenderIpOffset, data + kSenderIpOffset + 4, senderIp.begin());
    std::copy(data + kTargetIpOffset, data + kTargetIpOffset + 4, targetIp.begin());

    // 브로드캐스트/멀티캐스트 송신자 주소는 캐시에 넣을 수 없다.
    if ((senderMac[0] & 1) != 0) {
        return false;
    }

    MacAddress myMac{};
    IpAddress myIp{};
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        myMac = mac_;
        myIp = ip_;
        if (senderMac == myMac) {
            return false;
        }

        const bool forMe = myIp != kUnspecifiedIp && targetIp == myIp;
        const bool hasSenderIp = senderIp != kUnspecifiedIp && senderIp != myIp;

        // 이미 캐시에 있는 송신자는 갱신하고, 나를 향한 메시지의 송신자는 새로 추가한다.
        auto found = cache_.find(senderIp);
        if (hasSenderIp && (found != cache_.end() || forMe)) {
            CacheEntry& entry = cache_[senderIp];
            entry.ip = senderIp;
            entry.mac = senderMac;
            entry.state = EntryState::Complete;
            entry.expires = GetTickCount64() + kCompleteTimeoutMs;
            changed = true;
        }
    }

    if (senderIp == myIp && myIp != kUnspecifiedIp) {
        Status(L"IP 주소 충돌 감지: " + MappingText(senderIp, senderMac));
    }

    if (changed) {
        CacheChanged();
    }

    if (myIp == kUnspecifiedIp || targetIp != myIp) {
        return true;
    }

    if (operation == kRequest) {
        Status(L"ARP 요청 수신: " + MappingText(senderIp, senderMac) + L" (대상 " + Wire::IpText(targetIp) + L")");

        // 요청의 송신자 정보를 대상 쪽으로 옮기고(SWAPPING) 자기 주소를 송신자에 넣어 응답한다.
        const bool replied = SendArp(kReply, senderMac, senderMac, senderIp);
        Status(
            replied ? L"ARP 응답 송신: " + MappingText(myIp, myMac) + L" → " + Wire::IpText(senderIp)
                    : L"ARP 응답 송신 실패");
    } else {
        Status(L"ARP 응답 수신: " + MappingText(senderIp, senderMac));
    }
    return true;
}

void CARPLayer::CacheChanged() {
    if (onCacheChanged) {
        onCacheChanged();
    }
}
