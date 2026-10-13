#pragma once
#include "Protocol.h"

class CEthernetLayer : public CBaseLayer {
public:
    explicit CEthernetLayer(const char* name)
        : CBaseLayer(name) {
    }

    // GARP로 MAC을 바꿀 때 수신 스레드가 읽는 중일 수 있으므로 잠금으로 보호한다.
    void SetSourceAddress(const MacAddress& address) {
        std::lock_guard<std::mutex> lock(sourceMutex_);
        source_ = address;
    }

    MacAddress GetSourceAddress() const {
        std::lock_guard<std::mutex> lock(sourceMutex_);
        return source_;
    }

    // 지정한 목적지 MAC으로 보낸다. (ARP 브로드캐스트 요청, 유니캐스트 응답)
    bool SendPacket(const unsigned char*, size_t, uint16_t type, const MacAddress& destination);
    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;

    void OnIdle() override {
        for (int index = 0; GetUpperLayer(index) != nullptr; ++index) {
            GetUpperLayer(index)->OnIdle();
        }
    }

private:
    // 상위 레이어 연결 순서: ARP(0), IP(1)
    static int UpperIndex(uint16_t type);

    MacAddress source_{};
    mutable std::mutex sourceMutex_;
};
