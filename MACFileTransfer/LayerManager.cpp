#include "pch.h"
#include "LayerManager.h"

#include <sstream>

CBaseLayer* CLayerManager::GetLayer(const char* name) {
    for (CBaseLayer* layer : layers_) {
        if (std::string(layer->GetLayerName()) == name) {
            return layer;
        }
    }
    return nullptr;
}

void CLayerManager::ConnectLayers(const char* description) {
    std::istringstream input(description);
    std::string token;
    std::vector<CBaseLayer*> parentStack;
    CBaseLayer* currentLayer = nullptr;

    // 연결 기호:
    //   * : 양방향(상위/하위 모두 설정)
    //   + : 상위 레이어만 설정
    //   - : 하위 레이어만 설정
    while (input >> token) {
        if (token == "(") {
            if (currentLayer == nullptr) {
                throw std::logic_error("Missing layer");
            }
            parentStack.push_back(currentLayer);
            continue;
        }

        if (token == ")") {
            if (parentStack.empty()) {
                throw std::logic_error("Unbalanced layers");
            }
            parentStack.pop_back();
            continue;
        }

        if (currentLayer == nullptr) {
            currentLayer = GetLayer(token.c_str());
            if (currentLayer == nullptr) {
                throw std::logic_error("Unknown layer");
            }
            continue;
        }

        if (parentStack.empty()) {
            throw std::logic_error("Missing parent");
        }

        const char linkMode = token[0];
        currentLayer = GetLayer(token.c_str() + 1);
        if (currentLayer == nullptr) {
            throw std::logic_error("Unknown layer");
        }

        CBaseLayer* parentLayer = parentStack.back();
        switch (linkMode) {
        case '*':
            parentLayer->SetUpperUnderLayer(currentLayer);
            break;
        case '+':
            parentLayer->SetUpperLayer(currentLayer);
            break;
        case '-':
            parentLayer->SetUnderLayer(currentLayer);
            break;
        default:
            throw std::logic_error("Unknown link mode");
        }
    }

    if (!parentStack.empty()) {
        throw std::logic_error("Unbalanced layers");
    }
}
