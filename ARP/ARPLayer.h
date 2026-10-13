#pragma once
#include "EthernetLayer.h"

#include <unordered_map>

// IPv4 주소 4바이트를 32비트 정수로 묶어 해시 키로 쓴다.
struct IpAddressHash {
    size_t operator()(const IpAddress& ip) const {
        return std::hash<uint32_t>{}(Wire::Read32(ip.data()));
    }
};

// IP 주소 → Ethernet 주소를 동적으로 매핑하는 ARP 레이어.
// Ethernet 레이어 위에서 EtherType 0x0806 프레임을 송수신하고 ARP 캐시 테이블을 관리한다.
// 캐시 테이블은 IP 주소를 키로 하는 해시 테이블이다.
// Gratuitous ARP 송신과 Proxy ARP 테이블(다른 호스트 대신 응답)도 담당한다.
class CARPLayer : public CBaseLayer {
public:
    enum class EntryState {
        Incomplete,
        Complete,
    };

    struct CacheEntry {
        IpAddress ip{};
        MacAddress mac{};
        EntryState state = EntryState::Incomplete;
        ULONGLONG expires = 0;
    };

    // Proxy ARP 테이블 항목: 이 호스트가 대신 응답할 IP와 응답에 넣을 Ethernet 주소
    struct ProxyEntry {
        std::wstring device;
        IpAddress ip{};
        MacAddress mac{};
    };

    // 캐시 유효 시간: 완성 항목 20분, 미완성 항목 3분
    static constexpr ULONGLONG kCompleteTimeoutMs = 20ull * 60 * 1000;
    static constexpr ULONGLONG kIncompleteTimeoutMs = 3ull * 60 * 1000;

    static constexpr uint16_t kRequest = 1;
    static constexpr uint16_t kReply = 2;

    explicit CARPLayer(const char* name)
        : CBaseLayer(name) {
    }

    void SetSourceAddress(const MacAddress& mac, const IpAddress& ip) {
        std::lock_guard<std::mutex> lock(mutex_);
        mac_ = mac;
        ip_ = ip;
    }

    // 대상 IP로 ARP 요청을 브로드캐스트하고, 캐시에 없으면 Incomplete 항목을 만든다.
    bool SendRequest(const IpAddress& target);

    // 자기 IP를 대상으로 하는 ARP 요청(GARP)을 브로드캐스트한다.
    bool SendGratuitous();

    // Complete 항목이 있으면 MAC 주소를 돌려준다.
    bool Lookup(const IpAddress& target, MacAddress& mac);

    std::vector<CacheEntry> Snapshot();
    bool Remove(const IpAddress& target);
    void Clear();

    bool AddProxy(const ProxyEntry& entry);
    bool RemoveProxy(const IpAddress& ip);
    std::vector<ProxyEntry> ProxySnapshot();

    // 만료된 항목을 제거한다. 테스트에서 시각을 지정할 수 있도록 공개한다.
    void Expire(ULONGLONG now);

    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;

    void OnIdle() override {
        Expire(GetTickCount64());
    }

    // 캐시 테이블이 바뀌면 호출된다. (수신 스레드에서 호출될 수 있음)
    std::function<void()> onCacheChanged;

    // 다른 호스트가 내 IP를 쓰고 있으면 그 Ethernet 주소와 함께 호출된다. (수신 스레드)
    std::function<void(const MacAddress&)> onDuplicateIp;

private:
    bool SendArp(
        uint16_t operation,
        const MacAddress& ethernetDestination,
        const MacAddress& senderMac,
        const IpAddress& senderIp,
        const MacAddress& targetMac,
        const IpAddress& targetIp);
    void CacheChanged();

    std::mutex mutex_;
    MacAddress mac_{};
    IpAddress ip_{};
    std::unordered_map<IpAddress, CacheEntry, IpAddressHash> cache_;
    std::unordered_map<IpAddress, ProxyEntry, IpAddressHash> proxies_;
};
