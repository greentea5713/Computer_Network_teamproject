#include "pch.h"
#include "ChatAppLayer.h"

namespace {
constexpr uint16_t kExtendedLengthMarker = 0xFFFF;
constexpr unsigned char kExtendedLengthFlag = 0x01;
constexpr size_t kExtendedChatHeader = Wire::ChatHeader + sizeof(uint64_t);
constexpr size_t kMaximumPendingMessages = 32;
constexpr ULONGLONG kAssemblyTimeoutMs = 30'000;
}  // namespace

bool CChatAppLayer::StartSend(std::string text) {
    if (text.empty() || busy_) {
        return false;
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    cancel_ = false;
    busy_ = true;

    try {
        worker_ = std::thread([this, text = std::move(text)] {
            try {
                const bool sent = SendText(text);
                Status(
                    sent
                        ? L"채팅 프레임 송신 완료 (상대 수신 확인은 별도)"
                        : L"채팅 송신 중단 또는 실패");
            } catch (...) {
                Status(L"채팅 송신 자원 부족");
            }
            busy_ = false;
        });
    } catch (...) {
        busy_ = false;
        return false;
    }

    return true;
}

void CChatAppLayer::Stop() {
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    busy_ = false;
}

bool CChatAppLayer::SendText(const std::string& text) {
    auto* ethernetLayer = static_cast<CEthernetLayer*>(GetUnderLayer());
    if (ethernetLayer == nullptr) {
        return false;
    }
    const bool usesExtendedLength = text.size() > kExtendedLengthMarker;
    const uint16_t headerLength = usesExtendedLength ? kExtendedLengthMarker : static_cast<uint16_t>(text.size());
    const unsigned char headerFlag = usesExtendedLength ? kExtendedLengthFlag : 0;

    // 첫 조각에는 메시지 본문을 넣지 않고 전체 길이만 알린다.
    // 65,535바이트를 넘는 메시지는 뒤쪽 8바이트에 64비트 길이를 기록한다.
    std::vector<unsigned char> firstPacket( usesExtendedLength ? kExtendedChatHeader : Wire::ChatHeader, 0);
    Wire::Write16(firstPacket.data(), headerLength);
    firstPacket[2] = Wire::First;
    firstPacket[3] = headerFlag;
    if (usesExtendedLength) {
        Wire::Write64(firstPacket.data() + Wire::ChatHeader, text.size());
    }

    if (!ethernetLayer->SendPacket(
            firstPacket.data(),
            firstPacket.size(),
            Wire::ChatType)) {
        return false;
    }

    for (size_t offset = 0; offset < text.size();) {
        if (cancel_) {
            return false;
        }

        const size_t dataLength = std::min(Wire::ChatData, text.size() - offset);
        const bool isLast = offset + dataLength == text.size();

        std::vector<unsigned char> packet(Wire::ChatHeader + dataLength, 0);
        Wire::Write16(packet.data(), headerLength);
        packet[2] = isLast ? Wire::Last : Wire::Middle;
        packet[3] = headerFlag;
        std::copy(
            text.begin() + offset,
            text.begin() + offset + dataLength,
            packet.begin() + Wire::ChatHeader);

        if (!ethernetLayer->SendPacket(packet.data(), packet.size(), Wire::ChatType)) {
            return false;
        }

        offset += dataLength;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

void CChatAppLayer::OnIdle() {
    const ULONGLONG now = GetTickCount64();

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second.updated > kAssemblyTimeoutMs) {
            Status(L"채팅 재조립 시간 초과");
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CChatAppLayer::ReceiveFrame(
    const unsigned char* packet,
    size_t packetLength,
    const FrameContext& context) {
    if (packetLength < Wire::ChatHeader ||
        packet[2] > Wire::Last ||
        packet[3] > kExtendedLengthFlag) {
        return false;
    }
    const AssemblyKey key = std::make_pair(context.source, context.destination);
    const ULONGLONG now = GetTickCount64();
    OnIdle();

    if (packet[2] == Wire::First) {
        const bool usesExtendedLength = packet[3] == kExtendedLengthFlag;
        if (usesExtendedLength &&
            (packetLength < kExtendedChatHeader ||
             Wire::Read16(packet) != kExtendedLengthMarker)) {
            return false;
        }
        const uint64_t totalLength = usesExtendedLength
            ? Wire::Read64(packet + Wire::ChatHeader)
            : Wire::Read16(packet);

        // MFC 편집 컨트롤과 UTF 변환 API가 int 길이를 사용하므로 이를 상한으로 둔다.
        if (totalLength == 0 || totalLength > SIZE_MAX || totalLength > INT_MAX) {
            return false;
        }

        const bool isNewAssembly = pending_.find(key) == pending_.end();
        if (pending_.size() >= kMaximumPendingMessages && isNewAssembly) {
            return false;
        }
        Assembly assembly;
        assembly.total = totalLength;
        assembly.flag = packet[3];
        assembly.updated = now;
        pending_[key] = std::move(assembly);
        return true;
    }

    auto pendingIt = pending_.find(key);
    if (pendingIt == pending_.end()) {
        return false;
    }

    Assembly& assembly = pendingIt->second;
    const uint16_t expectedLength =
        assembly.flag == kExtendedLengthFlag
            ? kExtendedLengthMarker
            : static_cast<uint16_t>(assembly.total);

    if (packet[3] != assembly.flag || Wire::Read16(packet) != expectedLength) {
        pending_.erase(pendingIt);
        return false;
    }

    const uint64_t remaining = assembly.total - assembly.data.size();
    const size_t dataLength =
        static_cast<size_t>(std::min<uint64_t>(Wire::ChatData, remaining));
    const bool isLast = assembly.data.size() + dataLength == assembly.total;

    if (dataLength == 0 ||
        packetLength < Wire::ChatHeader + dataLength ||
        (packet[2] == Wire::Last) != isLast) {
        pending_.erase(pendingIt);
        Status(L"채팅 조각 길이 또는 순서 오류");
        return false;
    }

    assembly.data.insert(
        assembly.data.end(),
        packet + Wire::ChatHeader,
        packet + Wire::ChatHeader + dataLength);
    assembly.updated = now;

    if (!isLast) {
        return true;
    }

    CBaseLayer* upperLayer = GetUpperLayer(0);
    const bool delivered =
        upperLayer != nullptr &&
        upperLayer->ReceiveFrame(
            assembly.data.data(),
            assembly.data.size(),
            context);
    pending_.erase(pendingIt);
    return delivered;
}
