#pragma once
#include "ARPLayer.h"

// 자기 IP 주소를 보관하고, 목적지 IP의 Ethernet 주소 해석을 ARP 레이어에 요청한다.
// Ethernet 위에 ARP 레이어와 나란히 놓인다. (IP 데이터그램 송수신은 라우팅 실습에서 확장)
class CIPLayer : public CBaseLayer {
public:
    explicit CIPLayer(const char* name)
        : CBaseLayer(name) {
    }

    void SetArpLayer(CARPLayer* layer) {
        arp_ = layer;
    }

    void SetSourceAddress(const IpAddress& address) {
        source_ = address;
    }

    IpAddress GetSourceAddress() const { return source_; }

    // 사용자가 입력한 대상 IP의 ARP 요청을 보낸다.
    bool Request(const IpAddress& target) {
        return arp_ != nullptr && arp_->SendRequest(target);
    }

    // ARP 캐시를 먼저 찾고, 없으면 ARP 요청을 보낸 뒤 false를 돌려준다.
    bool Resolve(const IpAddress& target, MacAddress& mac) {
        if (arp_ == nullptr) {
            return false;
        }
        if (arp_->Lookup(target, mac)) {
            return true;
        }
        arp_->SendRequest(target);
        return false;
    }

private:
    CARPLayer* arp_ = nullptr;
    IpAddress source_{};
};
