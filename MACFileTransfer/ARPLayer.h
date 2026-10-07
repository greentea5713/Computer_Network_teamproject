#pragma once
#include "EthernetLayer.h"

// IP 주소 → Ethernet 주소를 동적으로 매핑하는 ARP 레이어.
// Ethernet 레이어 위에서 EtherType 0x0806 프레임을 송수신하고 ARP 캐시 테이블을 관리한다.
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

    // Complete 항목이 있으면 MAC 주소를 돌려준다.
    bool Lookup(const IpAddress& target, MacAddress& mac);

    std::vector<CacheEntry> Snapshot();
    bool Remove(const IpAddress& target);
    void Clear();

    // 만료된 항목을 제거한다. 테스트에서 시각을 지정할 수 있도록 공개한다.
    void Expire(ULONGLONG now);

    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;

    void OnIdle() override {
        Expire(GetTickCount64());
    }

    // 캐시 테이블이 바뀌면 호출된다. (수신 스레드에서 호출될 수 있음)
    std::function<void()> onCacheChanged;

private:
    bool SendArp(
        uint16_t operation,
        const MacAddress& ethernetDestination,
        const MacAddress& targetMac,
        const IpAddress& targetIp);
    void CacheChanged();

    std::mutex mutex_;
    MacAddress mac_{};
    IpAddress ip_{};
    std::map<IpAddress, CacheEntry> cache_;
};
