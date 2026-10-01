#pragma once
#include "EthernetLayer.h"

class CChatAppLayer : public CBaseLayer {
public:
    explicit CChatAppLayer(const char* name)
        : CBaseLayer(name) {
    }

    ~CChatAppLayer() override {
        Stop();
    }

    bool StartSend(std::string text);
    void Stop();

    bool Busy() const { return busy_; }

    void Reset() {
        pending_.clear();
    }

    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;
    void OnIdle() override;

private:
    struct Assembly {
        uint64_t total = 0;
        unsigned char flag = 0;
        std::vector<unsigned char> data;
        ULONGLONG updated = 0;
    };

    using AssemblyKey = std::pair<MacAddress, MacAddress>;

    bool SendText(const std::string&);

    std::map<AssemblyKey, Assembly> pending_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
    std::thread worker_;
};
