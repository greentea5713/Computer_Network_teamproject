#pragma once
#include "RouterCore.h"
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
    void ClearPending() { std::lock_guard<std::mutex> guard(mutex_); pending_.clear(); }
    bool Receive(const unsigned char* data, size_t length) {
        std::vector<unsigned char> packet;
        if (!RouterCore::Prepare(data, length, packet)) return false;
        IpAddress destination{};
        std::copy(data + 16, data + 20, destination.begin());
        if (destination == ports[0].ip || destination == ports[1].ip ||
            destination[0] == 0 || destination[0] >= 224 || destination == IpAddress{255,255,255,255}) return false;
        RouteEntry route;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!RouterCore::Select(routes_, destination, route)) return false;
        }
        const IpAddress next = route.viaGateway ? route.gateway : destination;
        MacAddress mac{};
        Port& port = ports[route.port];
        if (port.arp->Lookup(next, mac)) return port.ethernet->SendPacket(packet.data(), packet.size(), Wire::IpType, mac);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (pending_.size() >= 128) pending_.pop_front();
            pending_.push_back({route.port, next, std::move(packet), GetTickCount64()});
        }
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
};

class RouterInput : public CBaseLayer {
public:
    RouterInput(const char* name, Router& router) : CBaseLayer(name), router_(router) {}
    bool ReceiveFrame(const unsigned char* data, size_t length, const FrameContext&) override {
        return router_.Receive(data, length);
    }
private:
    Router& router_;
};
