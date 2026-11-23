#pragma once
#include "NatCore.h"
#include "ARPLayer.h"

class Router {
public:
    struct Port { CEthernetLayer* ethernet = nullptr; CARPLayer* arp = nullptr; IpAddress ip{}; };
    Port ports[2];

    void SetRoutes(const std::vector<RouteEntry>& routes) {
        std::lock_guard<std::mutex> guard(mutex_);
        routes_ = routes;
        pending_.clear();
    }
    void Reset() {
        std::lock_guard<std::mutex> guard(mutex_);
        pending_.clear();
        nat_.ClearMappings();
    }
    bool AddForward(const NatCore::Rule& rule) {
        std::lock_guard<std::mutex> guard(mutex_);
        nat_.Expire(GetTickCount64());
        return nat_.AddRule(rule);
    }
    void DeleteForward(unsigned char protocol, uint16_t outside) {
        std::lock_guard<std::mutex> guard(mutex_);
        nat_.DeleteRule(protocol, outside);
        pending_.clear();
    }
    std::vector<NatCore::Mapping> NatSnapshot() {
        std::lock_guard<std::mutex> guard(mutex_);
        nat_.Expire(GetTickCount64());
        return nat_.Mappings();
    }
    bool Receive(int ingress, const unsigned char* data, size_t length) {
        std::vector<unsigned char> packet;
        if (!RouterCore::Prepare(data, length, packet)) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        IpAddress destination{};
        RouteEntry route;
        const auto now = GetTickCount64();
        if (!nat_.Forward(packet, ingress, ports[NatCore::Inside].ip, ports[NatCore::Outside].ip,
                          routes_, now, route, destination)) return false;
        const IpAddress next = route.viaGateway ? route.gateway : destination;
        MacAddress mac{};
        Port& port = ports[route.port];
        if (port.arp->Lookup(next, mac)) return port.ethernet->SendPacket(packet.data(), packet.size(), Wire::IpType, mac);
        if (pending_.size() >= 128) pending_.pop_front();
        pending_.push_back({route.port, next, std::move(packet), now});
        port.arp->SendRequest(next);
        return true;
    }
    void Flush() {
        std::lock_guard<std::mutex> guard(mutex_);
        const ULONGLONG now = GetTickCount64();
        for (auto it = pending_.begin(); it != pending_.end();) {
            MacAddress mac{};
            Port& port = ports[it->port];
            if (now - it->created > 3000) it = pending_.erase(it);
            else if (port.arp->Lookup(it->next, mac)) {
                port.ethernet->SendPacket(it->packet.data(), it->packet.size(), Wire::IpType, mac);
                it = pending_.erase(it);
            } else ++it;
        }
    }
private:
    struct Pending { int port; IpAddress next; std::vector<unsigned char> packet; ULONGLONG created; };
    std::mutex mutex_;
    std::vector<RouteEntry> routes_;
    std::deque<Pending> pending_;
    NatCore::Table nat_;
};

class RouterInput : public CBaseLayer {
public:
    RouterInput(const char* name, Router& router, int ingress)
        : CBaseLayer(name), router_(router), ingress_(ingress) {}
    bool ReceiveFrame(const unsigned char* data, size_t length, const FrameContext&) override {
        return router_.Receive(ingress_, data, length);
    }
private:
    Router& router_;
    int ingress_;
};
