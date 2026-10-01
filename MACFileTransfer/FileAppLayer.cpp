#include "pch.h"
#include "FileAppLayer.h"

namespace {

constexpr uint32_t kExtendedSizeMarker = UINT32_MAX;
constexpr unsigned char kExtendedSizeFlag = 0x01;
constexpr size_t kExtendedSizeBytes = sizeof(uint64_t);
constexpr size_t kMaximumIncomingFiles = 16;
constexpr ULONGLONG kTransferTimeoutMs = 30'000;
constexpr unsigned int kMaximumNameAttempts = 100;

struct FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~FileHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

bool IsSafeFileName(const std::wstring& name) {
    if (name.empty() ||
        name.size() > 180 ||
        name == L"." ||
        name == L".." ||
        name.back() == L'.' ||
        name.back() == L' ') {
        return false;
    }

    constexpr wchar_t kInvalidCharacters[] = L"\\/:*?\"<>|";
    for (wchar_t character : name) {
        if (character < 32 ||
            std::wstring(kInvalidCharacters).find(character) != std::wstring::npos) {
            return false;
        }
    }

    return true;
}

uint64_t FragmentCount(uint64_t fileSize) {
    return fileSize / Wire::FileData + (fileSize % Wire::FileData != 0);
}

}  // namespace

bool CFileAppLayer::StartSend(const std::wstring& path) {
    if (busy_ || path.empty()) {
        return false;
    }

    JoinWorker();
    cancel_ = false;
    busy_ = true;

    auto context = std::make_unique<FileTransferContext>();
    context->layer = this;
    context->path = path;

    worker_ = AfxBeginThread(
        &CFileAppLayer::FileTransferThreadEntry,
        context.get(),
        THREAD_PRIORITY_NORMAL,
        0,
        CREATE_SUSPENDED);
    if (worker_ == nullptr) {
        busy_ = false;
        return false;
    }

    worker_->m_bAutoDelete = FALSE;
    context.release();  // 전송 스레드가 unique_ptr로 소유권을 넘겨받는다.
    worker_->ResumeThread();
    return true;
}

void CFileAppLayer::JoinWorker() {
    if (worker_ == nullptr) {
        return;
    }

    WaitForSingleObject(worker_->m_hThread, INFINITE);
    delete worker_;
    worker_ = nullptr;
}

void CFileAppLayer::Stop() {
    cancel_ = true;
    JoinWorker();
    busy_ = false;
}

void CFileAppLayer::Reset() {
    for (const auto& entry : incoming_) {
        const auto& incoming = *entry.second;
        ReportProgress(false, incoming.total ? static_cast<int>(incoming.received * 100 / incoming.total) : 0, ProgressState::Failed);
    }
    incoming_.clear();
}

UINT AFX_CDECL CFileAppLayer::FileTransferThreadEntry(LPVOID parameter) {
    return FileTransferThread(reinterpret_cast<LPDWORD>(parameter));
}

UINT AFX_CDECL CFileAppLayer::FileTransferThread(LPDWORD lpdwParam) {
    std::unique_ptr<FileTransferContext> context(
        reinterpret_cast<FileTransferContext*>(lpdwParam));
    if (!context || context->layer == nullptr) {
        return 1;
    }

    CFileAppLayer* layer = context->layer;
    bool sent = false;

    try {
        layer->sentPercent_ = 0;
        layer->ReportProgress(true, 0);
        sent = layer->SendFileFrames(context->path);
        layer->Status(
            sent
                ? L"파일 프레임 송신 완료 (상대 수신 확인은 별도)"
                : L"파일 송신 중단 또는 실패");
    } catch (...) {
        layer->Status(L"파일 송신 처리 실패");
    }

    layer->ReportProgress(true, sent ? 100 : layer->sentPercent_, sent ? ProgressState::Complete : ProgressState::Failed);
    layer->busy_ = false;
    return sent ? 0 : 1;
}

bool CFileAppLayer::SendFileFrames(const std::wstring& path) {
    auto* ethernetLayer = static_cast<CEthernetLayer*>(GetUnderLayer());
    if (ethernetLayer == nullptr) {
        return false;
    }

    FileHandle file;
    file.handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) {
        Status(L"선택한 파일을 열 수 없습니다.");
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file.handle, &fileSize) || fileSize.QuadPart < 0) {
        return false;
    }

    const uint64_t totalSize = static_cast<uint64_t>(fileSize.QuadPart);
    // 조각 번호는 32비트이므로 범위를 초과할 때 조용히 순환시키지 않는다.
    if (FragmentCount(totalSize) > UINT32_MAX - 1ULL) {
        Status(L"32비트 조각 번호 범위를 초과합니다.");
        return false;
    }

    const std::wstring baseName = path.substr(path.find_last_of(L"\\/") + 1);
    const std::string utf8Name = Wire::Utf8(baseName);
    if (!IsSafeFileName(baseName) ||
        utf8Name.size() + kExtendedSizeBytes + 1 > Wire::FileData) {
        return false;
    }

    const bool usesExtendedSize = totalSize > kExtendedSizeMarker;
    const uint32_t headerSize =
        usesExtendedSize ? kExtendedSizeMarker : static_cast<uint32_t>(totalSize);
    const unsigned char headerFlag = usesExtendedSize ? kExtendedSizeFlag : 0;

    const auto SendPacket =
        [&](unsigned char type,
            uint32_t sequence,
            const unsigned char* data,
            size_t dataLength) {
            std::vector<unsigned char> packet(Wire::FileHeader + dataLength, 0);
            Wire::Write32(packet.data(), headerSize);
            Wire::Write16(packet.data() + 4, type);
            packet[6] = 0;
            packet[7] = headerFlag;
            Wire::Write32(packet.data() + 8, sequence);

            if (dataLength > 0) {
                std::copy(
                    data,
                    data + dataLength,
                    packet.begin() + Wire::FileHeader);
            }

            return ethernetLayer->SendPacket(
                packet.data(),
                packet.size(),
                Wire::FileType);
        };

    // 첫 조각에는 파일 크기와 UTF-8 파일명을 담고, 실제 파일 데이터는 중간 조각부터 보낸다.
    std::vector<unsigned char> metadata(usesExtendedSize ? kExtendedSizeBytes : 0);
    if (usesExtendedSize) {
        Wire::Write64(metadata.data(), totalSize);
    }
    metadata.insert(metadata.end(), utf8Name.begin(), utf8Name.end());
    metadata.push_back(0);

    if (!SendPacket(Wire::First, 0, metadata.data(), metadata.size())) {
        return false;
    }

    uint64_t sentBytes = 0;
    uint32_t sequence = 1;
    unsigned char buffer[Wire::FileData];
    int lastReportedPercent = -1;

    while (sentBytes < totalSize) {
        if (cancel_) {
            return false;
        }

        const DWORD bytesToRead = static_cast<DWORD>(
            std::min<uint64_t>(Wire::FileData, totalSize - sentBytes));
        DWORD bytesRead = 0;
        if (!ReadFile(
                file.handle,
                buffer,
                bytesToRead,
                &bytesRead,
                nullptr) ||
            bytesRead != bytesToRead) {
            return false;
        }

        if (!SendPacket(Wire::Middle, sequence++, buffer, bytesRead)) {
            return false;
        }

        sentBytes += bytesRead;
        const int percent = static_cast<int>(sentBytes * 100 / totalSize);
        if (percent != lastReportedPercent) {
            lastReportedPercent = percent;
            sentPercent_ = percent;
            ReportProgress(true, percent);
            Status(L"파일 송신 " + std::to_wstring(percent) + L"%");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return !cancel_ && SendPacket(Wire::Last, sequence, nullptr, 0);
}

void CFileAppLayer::OnIdle() {
    const ULONGLONG now = GetTickCount64();

    for (auto it = incoming_.begin(); it != incoming_.end();) {
        if (now - it->second->updated > kTransferTimeoutMs) {
            const auto& incoming = *it->second;
            ReportProgress(false, incoming.total ? static_cast<int>(incoming.received * 100 / incoming.total) : 0, ProgressState::Failed);
            Status(L"파일 수신 시간 초과: 임시 파일 제거");
            it = incoming_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CFileAppLayer::ReceiveFrame(
    const unsigned char* packet,
    size_t packetLength,
    const FrameContext& context) {
    if (packetLength < Wire::FileHeader ||
        packet[6] != 0 ||
        packet[7] > kExtendedSizeFlag) {
        return false;
    }

    const uint16_t type = Wire::Read16(packet + 4);
    const uint32_t sequence = Wire::Read32(packet + 8);
    const TransferKey key = std::make_pair(context.source, context.destination);
    const ULONGLONG now = GetTickCount64();
    OnIdle();

    if (type == Wire::First) {
        const bool usesExtendedSize = packet[7] == kExtendedSizeFlag;
        const size_t nameOffset =
            Wire::FileHeader + (usesExtendedSize ? kExtendedSizeBytes : 0);

        if (sequence != 0 ||
            packetLength <= nameOffset ||
            (usesExtendedSize && Wire::Read32(packet) != kExtendedSizeMarker)) {
            Status(L"파일 메타데이터 헤더 오류");
            return false;
        }

        const unsigned char* nameEnd =
            std::find(packet + nameOffset, packet + packetLength, 0);
        if (nameEnd == packet + packetLength) {
            Status(L"파일명 종료 문자가 없습니다.");
            return false;
        }

        const std::wstring fileName = Wire::Wide(std::string(
            reinterpret_cast<const char*>(packet + nameOffset),
            reinterpret_cast<const char*>(nameEnd)));
        if (!IsSafeFileName(fileName)) {
            Status(L"수신 파일명이 올바르지 않습니다.");
            return false;
        }

        const uint64_t totalSize = usesExtendedSize
            ? Wire::Read64(packet + Wire::FileHeader)
            : Wire::Read32(packet);
        if (FragmentCount(totalSize) > UINT32_MAX - 1ULL) {
            Status(L"수신 파일 조각 수가 범위를 초과합니다.");
            return false;
        }

        const bool isNewTransfer = incoming_.find(key) == incoming_.end();
        if (incoming_.size() >= kMaximumIncomingFiles && isNewTransfer) {
            Status(L"동시 파일 수신 한도를 초과했습니다.");
            return false;
        }

        incoming_.erase(key);
        auto incoming = std::make_unique<Incoming>();
        incoming->total = totalSize;
        incoming->flag = packet[7];
        incoming->updated = now;

        if (!CreateDirectoryW(directory_.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            Status(L"수신 폴더 생성 실패");
            return false;
        }

        // 접두사로 Windows 예약 장치명을 피하고 CREATE_NEW로 기존 파일 덮어쓰기를 막는다.
        for (unsigned int attempt = 0; attempt < kMaximumNameAttempts; ++attempt) {
            incoming->finalPath =
                directory_ +
                L"\\received_" +
                std::to_wstring(GetTickCount64()) +
                L"_" +
                std::to_wstring(attempt) +
                L"_" +
                fileName;

            if (GetFileAttributesW(incoming->finalPath.c_str()) !=
                INVALID_FILE_ATTRIBUTES) {
                continue;
            }

            incoming->partial = incoming->finalPath + L".part";
            incoming->file = CreateFileW(
                incoming->partial.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (incoming->file != INVALID_HANDLE_VALUE) {
                break;
            }
            incoming->partial.clear();
        }

        if (incoming->file == INVALID_HANDLE_VALUE) {
            Status(
                L"수신 파일 생성 실패 (Windows 오류 " +
                std::to_wstring(GetLastError()) +
                L"): 저장 경로 권한을 확인하세요.");
            return false;
        }

        ULARGE_INTEGER availableBytes{};
        if (!GetDiskFreeSpaceExW(
                directory_.c_str(),
                &availableBytes,
                nullptr,
                nullptr) ||
            totalSize > availableBytes.QuadPart) {
            Status(L"수신 디스크 공간 부족");
            return false;
        }

        // 수신 전에 전체 논리 크기를 확보하여 공간 부족을 전송 도중이 아닌 시작 시 확인한다.
        if (totalSize > static_cast<uint64_t>(INT64_MAX)) {
            Status(L"수신 파일 크기가 Windows 파일 범위를 초과합니다.");
            return false;
        }

        LARGE_INTEGER fileEnd{};
        fileEnd.QuadPart = static_cast<LONGLONG>(totalSize);
        LARGE_INTEGER fileBegin{};
        if (!SetFilePointerEx(
                incoming->file,
                fileEnd,
                nullptr,
                FILE_BEGIN) ||
            !SetEndOfFile(incoming->file) ||
            !SetFilePointerEx(
                incoming->file,
                fileBegin,
                nullptr,
                FILE_BEGIN)) {
            Status(
                L"수신 파일 공간 확보 실패 (Windows 오류 " +
                std::to_wstring(GetLastError()) +
                L")");
            return false;
        }

        Status(L"파일 수신 시작: " + fileName);
        incoming_[key] = std::move(incoming);
        ReportProgress(false, 0);
        return true;
    }

    auto incomingIt = incoming_.find(key);
    if (incomingIt == incoming_.end()) {
        return false;
    }

    Incoming& incoming = *incomingIt->second;
    const auto FailTransfer = [&]() {
        ReportProgress(false, incoming.total ? static_cast<int>(incoming.received * 100 / incoming.total) : 0, ProgressState::Failed);
        incoming_.erase(incomingIt);
        Status(L"파일 조각 순서 또는 길이 오류: 임시 파일 제거");
        return false;
    };

    const uint32_t expectedSize =
        incoming.flag == kExtendedSizeFlag
            ? kExtendedSizeMarker
            : static_cast<uint32_t>(incoming.total);
    if (sequence != incoming.sequence ||
        packet[7] != incoming.flag ||
        Wire::Read32(packet) != expectedSize) {
        return FailTransfer();
    }

    if (type == Wire::Middle) {
        const DWORD dataLength = static_cast<DWORD>(
            std::min<uint64_t>(
                Wire::FileData,
                incoming.total - incoming.received));
        DWORD writtenBytes = 0;

        if (dataLength == 0 ||
            packetLength < Wire::FileHeader + dataLength ||
            !WriteFile(
                incoming.file,
                packet + Wire::FileHeader,
                dataLength,
                &writtenBytes,
                nullptr) ||
            writtenBytes != dataLength) {
            return FailTransfer();
        }

        const int previousPercent = incoming.total
            ? static_cast<int>(incoming.received * 100 / incoming.total)
            : 100;
        incoming.received += writtenBytes;
        ++incoming.sequence;
        incoming.updated = now;

        const int currentPercent = incoming.total
            ? static_cast<int>(incoming.received * 100 / incoming.total)
            : 100;
        if (currentPercent != previousPercent) {
            ReportProgress(false, currentPercent);
            Status(L"파일 수신 " + std::to_wstring(currentPercent) + L"%");
        }
        return true;
    }

    if (type != Wire::Last || incoming.received != incoming.total) {
        return FailTransfer();
    }

    LARGE_INTEGER actualSize{};
    if (!GetFileSizeEx(incoming.file, &actualSize) ||
        static_cast<uint64_t>(actualSize.QuadPart) != incoming.total ||
        !FlushFileBuffers(incoming.file)) {
        return FailTransfer();
    }

    CloseHandle(incoming.file);
    incoming.file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(
            incoming.partial.c_str(),
            incoming.finalPath.c_str(),
            MOVEFILE_WRITE_THROUGH)) {
        return FailTransfer();
    }

    incoming.partial.clear();
    ReportProgress(false, 100, ProgressState::Complete);
    Status(L"파일 수신 완료: " + incoming.finalPath);
    incoming_.erase(incomingIt);
    return true;
}
