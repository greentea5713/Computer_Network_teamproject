#pragma once
#include "pch.h"

using MacAddress = std::array<unsigned char, 6>;

// 상위 레이어가 Ethernet 헤더를 다시 해석하지 않도록 주소 정보를 함께 전달한다.
struct FrameContext {
    MacAddress source{};
    MacAddress destination{};
};

class CBaseLayer {
public:
    explicit CBaseLayer(const char* name)
        : name_(name) {
    }

    virtual ~CBaseLayer() = default;

    const char* GetLayerName() const { return name_.c_str(); }
    CBaseLayer* GetUnderLayer() { return under_; }

    CBaseLayer* GetUpperLayer(int index) {
        const bool validIndex = index >= 0 && index < static_cast<int>(upper_.size());
        return validIndex ? upper_[index] : nullptr;
    }

    void SetUnderLayer(CBaseLayer* layer) {
        under_ = layer;
    }

    void SetUpperLayer(CBaseLayer* layer) {
        if (layer != nullptr) {
            upper_.push_back(layer);
        }
    }

    void SetUpperUnderLayer(CBaseLayer* layer) {
        SetUpperLayer(layer);
        if (layer != nullptr) {
            layer->SetUnderLayer(this);
        }
    }

    void SetUnderUpperLayer(CBaseLayer* layer) {
        SetUnderLayer(layer);
        if (layer != nullptr) {
            layer->SetUpperLayer(this);
        }
    }

    virtual BOOL Send(unsigned char*, int) { return FALSE; }
    virtual BOOL Receive(unsigned char*) { return FALSE; }
    virtual BOOL Receive() { return FALSE; }
    virtual bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) { return false; }
    virtual void OnIdle() {}

    std::function<void(const std::wstring&)> notify;

protected:
    void Status(const std::wstring& message) {
        if (notify) {
            notify(message);
        }
    }

private:
    std::string name_;
    CBaseLayer* under_ = nullptr;
    std::vector<CBaseLayer*> upper_;
};
