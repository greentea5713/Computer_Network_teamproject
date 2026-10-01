#include "pch.h"
#include "EthernetLayer.h"

namespace {
constexpr size_t kMinimumFrameSizeWithoutFcs = 60;
constexpr size_t kMaximumFrameSizeWithoutFcs = Wire::EthernetHeader + Wire::MTU;
}  // namespace

bool CEthernetLayer::SendPacket(const unsigned char* data, size_t length, uint16_t type) {
    if (data == nullptr || length > Wire::MTU || GetUnderLayer() == nullptr) {
        return false;
    }

    // Ethernet 최소 프레임 크기는 FCS를 제외하면 60바이트이다.
    // FCS, preamble, SFD는 NIC가 덧붙이므로 프로그램에서는 생성하지 않는다.
    const size_t frameSize =
        std::max(kMinimumFrameSizeWithoutFcs, Wire::EthernetHeader + length);
    std::vector<unsigned char> frame(frameSize, 0);

    std::copy(destination_.begin(), destination_.end(), frame.begin());
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

    // 자신이 보낸 프레임과 자신에게 오지 않은 프레임을 상위 레이어에 전달하지 않는다.
    if (context.source == source_ || context.destination != source_) {
        return false;
    }

    const uint16_t type = Wire::Read16(frame + 12);
    int upperIndex = -1;
    if (type == Wire::ChatType) {
        upperIndex = 0;
    } else if (type == Wire::FileType) {
        upperIndex = 1;
    }

    CBaseLayer* upperLayer = GetUpperLayer(upperIndex);
    return upperLayer != nullptr &&
        upperLayer->ReceiveFrame(
            frame + Wire::EthernetHeader,
            length - Wire::EthernetHeader,
            context);
}
