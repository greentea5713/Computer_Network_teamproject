#pragma once
#include "BaseLayer.h"

// 실제 레이어 객체의 소유권은 Dialog에 있다.
// CLayerManager는 이름 검색과 레이어 연결만 담당한다.
class CLayerManager {
public:
    void AddLayer(CBaseLayer* layer) {
        layers_.push_back(layer);
    }

    CBaseLayer* GetLayer(const char* name);

    CBaseLayer* GetLayer(int index) {
        const bool validIndex = index >= 0 && index < static_cast<int>(layers_.size());
        return validIndex ? layers_[index] : nullptr;
    }

    void ConnectLayers(const char* description);

private:
    std::vector<CBaseLayer*> layers_;
};
