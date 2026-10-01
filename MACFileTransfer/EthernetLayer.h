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

    void SetDestinAddress(const MacAddress& address) {
        destination_ = address;
    }

    MacAddress GetSourceAddress() const { return source_; }
    MacAddress GetDestinAddress() const { return destination_; }

    bool SendPacket(const unsigned char*, size_t, uint16_t);
    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;

    void OnIdle() override {
        for (int index = 0; index < 2; ++index) {
            CBaseLayer* upperLayer = GetUpperLayer(index);
            if (upperLayer != nullptr) {
                upperLayer->OnIdle();
            }
        }
    }

private:
    MacAddress source_{};
    MacAddress destination_{};
};
