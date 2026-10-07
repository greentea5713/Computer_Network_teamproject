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

    // 채팅/파일 송신 스레드가 읽는 중에도 UI에서 목적지를 바꿀 수 있도록 잠금으로 보호한다.
    void SetDestinAddress(const MacAddress& address) {
        std::lock_guard<std::mutex> lock(destinationMutex_);
        destination_ = address;
    }

    MacAddress GetSourceAddress() const { return source_; }

    MacAddress GetDestinAddress() const {
        std::lock_guard<std::mutex> lock(destinationMutex_);
        return destination_;
    }

    // 설정된 목적지 MAC으로 보낸다. (채팅/파일)
    bool SendPacket(const unsigned char*, size_t, uint16_t);
    // 지정한 목적지 MAC으로 보낸다. (ARP 브로드캐스트 요청, 유니캐스트 응답)
    bool SendPacket(const unsigned char*, size_t, uint16_t, const MacAddress&);
    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;

    void OnIdle() override {
        for (int index = 0; GetUpperLayer(index) != nullptr; ++index) {
            GetUpperLayer(index)->OnIdle();
        }
    }

private:
    // 상위 레이어 연결 순서: ChatApp(0), FileApp(1), ARP(2), IP(3)
    static int UpperIndex(uint16_t type);

    MacAddress source_{};
    MacAddress destination_{};
    mutable std::mutex destinationMutex_;
};
