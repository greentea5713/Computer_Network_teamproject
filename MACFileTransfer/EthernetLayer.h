#pragma once
#include "Protocol.h"

class CEthernetLayer : public CBaseLayer {
public:
    explicit CEthernetLayer(const char* name)
        : CBaseLayer(name) {
    }

    void SetSourceAddress(const MacAddress& address) {
        source_ = address;
    }

    MacAddress GetSourceAddress() const { return source_; }

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
};
