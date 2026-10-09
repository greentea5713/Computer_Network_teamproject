#include "pch.h"
#include "EthernetLayer.h"

namespace {
constexpr size_t kMinimumFrameSizeWithoutFcs = 60;
constexpr size_t kMaximumFrameSizeWithoutFcs = Wire::EthernetHeader + Wire::MTU;
}  // namespace

int CEthernetLayer::UpperIndex(uint16_t type) {
    switch (type) {
    case Wire::ArpType:
        return 0;
    case Wire::IpType:
        return 1;
    default:
        return -1;
    }
}

bool CEthernetLayer::SendPacket(
    const unsigned char* data,
    size_t length,
    uint16_t type,
    const MacAddress& destination) {
    if (data == nullptr || length > Wire::MTU || GetUnderLayer() == nullptr) {
        return false;
    }

    // Ethernet 최소 프레임 크기는 FCS를 제외하면 60바이트이다.
    // FCS, preamble, SFD는 NIC가 덧붙이므로 프로그램에서는 생성하지 않는다.
    const size_t frameSize =
        std::max(kMinimumFrameSizeWithoutFcs, Wire::EthernetHeader + length);
    std::vector<unsigned char> frame(frameSize, 0);

    std::copy(destination.begin(), destination.end(), frame.begin());
    std::copy(source_.begin(), source_.end(), frame.begin() + 6);
    Wire::Write16(frame.data() + 12, type);
    std::copy(data, data + length, frame.begin() + Wire::EthernetHeader);

    return GetUnderLayer()->Send(frame.data(), static_cast<int>(frame.size())) != FALSE;
}

bool CEthernetLayer::ReceiveFrame(
    const unsigned char* frame,
    size_t length,
    const FrameContext&) {
    if (frame == nullptr || length < Wire::EthernetHeader ||
        length > kMaximumFrameSizeWithoutFcs) {
        return false;
    }

    FrameContext context;
    std::copy(frame, frame + 6, context.destination.begin());
    std::copy(frame + 6, frame + 12, context.source.begin());
    const uint16_t type = Wire::Read16(frame + 12);

    // 자신이 보낸 프레임은 버린다.
    // 목적지는 자기 MAC이어야 하며, 브로드캐스트는 ARP 요청을 받기 위해 ARP만 허용한다.
    const bool toMe = context.destination == source_;
    const bool arpBroadcast =
        context.destination == Wire::BroadcastMac && type == Wire::ArpType;
    if (context.source == source_ || (!toMe && !arpBroadcast)) {
        return false;
    }

    CBaseLayer* upperLayer = GetUpperLayer(UpperIndex(type));
    return upperLayer != nullptr &&
        upperLayer->ReceiveFrame(
            frame + Wire::EthernetHeader,
            length - Wire::EthernetHeader,
            context);
}
