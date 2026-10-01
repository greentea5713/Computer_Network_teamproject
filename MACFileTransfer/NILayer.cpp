#include "pch.h"
#include "NILayer.h"

#include <Packet32.h>

namespace {
constexpr ULONG kOidCurrentEthernetAddress = 0x01010102;
constexpr int kCaptureSnapshotLength = 65'536;
constexpr int kReadTimeoutMs = 100;
constexpr size_t kMaximumFrameSizeWithoutFcs = Wire::EthernetHeader + Wire::MTU;
constexpr char kProtocolFilter[] = "ether proto 0x2080 or ether proto 0x2090";
}  // namespace

std::vector<NetworkAdapter> CNILayer::Enumerate() {
    std::vector<NetworkAdapter> result;
    pcap_if_t* devices = nullptr;
    char error[PCAP_ERRBUF_SIZE]{};

    if (pcap_findalldevs(&devices, error) < 0) {
        Status(L"어댑터 열거 실패: " + Wire::Wide(error));
        return result;
    }

    for (pcap_if_t* device = devices; device != nullptr; device = device->next) {
        NetworkAdapter networkAdapter;
        networkAdapter.name = device->name;
        networkAdapter.description =
            Wire::Wide(device->description ? device->description : device->name);

        // Packet Driver API의 OID_802_3_CURRENT_ADDRESS로 실제 MAC 주소를 조회한다.
        LPADAPTER packetAdapter =
            PacketOpenAdapter(const_cast<char*>(networkAdapter.name.c_str()));
        if (packetAdapter != nullptr) {
            alignas(PACKET_OID_DATA)
                unsigned char oidBuffer[sizeof(PACKET_OID_DATA) + 6]{};
            auto* oidData = reinterpret_cast<PPACKET_OID_DATA>(oidBuffer);
            oidData->Oid = kOidCurrentEthernetAddress;
            oidData->Length = 6;

            if (PacketRequest(packetAdapter, FALSE, oidData) && oidData->Length == 6) {
                std::copy(
                    oidData->Data,
                    oidData->Data + 6,
                    networkAdapter.mac.begin());

                const bool isNonZero = std::any_of(
                    networkAdapter.mac.begin(),
                    networkAdapter.mac.end(),
                    [](unsigned char byte) { return byte != 0; });
                const bool isUnicast = (networkAdapter.mac[0] & 1) == 0;
                networkAdapter.hasMac = isNonZero && isUnicast;
            }

            PacketCloseAdapter(packetAdapter);
        }

        result.push_back(networkAdapter);
    }

    pcap_freealldevs(devices);
    return result;
}

bool CNILayer::Open(const NetworkAdapter& adapter) {
    Close();

    char error[PCAP_ERRBUF_SIZE]{};
    rx_ = pcap_open_live(
        adapter.name.c_str(),
        kCaptureSnapshotLength,
        1,
        kReadTimeoutMs,
        error);
    if (rx_ == nullptr) {
        Status(L"수신 장치 열기 실패: " + Wire::Wide(error));
        return false;
    }

    tx_ = pcap_open_live(
        adapter.name.c_str(),
        kCaptureSnapshotLength,
        0,
        kReadTimeoutMs,
        error);
    if (tx_ == nullptr ||
        pcap_datalink(rx_) != DLT_EN10MB ||
        pcap_datalink(tx_) != DLT_EN10MB) {
        Status(L"Ethernet 장치를 열 수 없습니다. 유선 NIC와 Npcap 설정을 확인하세요.");
        Close();
        return false;
    }

    // 필요한 두 EtherType만 커널 필터에서 통과시켜 수신 스레드의 부하를 줄인다.
    bpf_program filter{};
    if (pcap_compile(
            rx_,
            &filter,
            kProtocolFilter,
            1,
            PCAP_NETMASK_UNKNOWN) < 0) {
        Close();
        return false;
    }

    const int setFilterResult = pcap_setfilter(rx_, &filter);
    pcap_freecode(&filter);
    if (setFilterResult < 0 || pcap_setnonblock(rx_, 1, error) < 0) {
        Close();
        return false;
    }

    stop_ = false;
    reader_ = AfxBeginThread(
        &CNILayer::ReadingThreadEntry,
        this,
        THREAD_PRIORITY_NORMAL,
        0,
        CREATE_SUSPENDED);
    if (reader_ == nullptr) {
        Close();
        Status(L"수신 스레드 생성 실패");
        return false;
    }

    reader_->m_bAutoDelete = FALSE;
    reader_->ResumeThread();
    return true;
}

void CNILayer::JoinReader() {
    if (reader_ == nullptr) {
        return;
    }

    WaitForSingleObject(reader_->m_hThread, INFINITE);
    delete reader_;
    reader_ = nullptr;
}

void CNILayer::Close() {
    stop_ = true;
    if (rx_ != nullptr) {
        pcap_breakloop(rx_);
    }

    JoinReader();

    std::lock_guard<std::mutex> lock(sendMutex_);
    if (rx_ != nullptr) {
        pcap_close(rx_);
    }
    if (tx_ != nullptr) {
        pcap_close(tx_);
    }
    rx_ = nullptr;
    tx_ = nullptr;
}

BOOL CNILayer::Send(unsigned char* data, int length) {
    std::lock_guard<std::mutex> lock(sendMutex_);

    if (stop_ ||
        tx_ == nullptr ||
        length < static_cast<int>(Wire::EthernetHeader) ||
        length > static_cast<int>(kMaximumFrameSizeWithoutFcs)) {
        return FALSE;
    }

    if (pcap_sendpacket(tx_, data, length) != 0) {
        Status(L"프레임 송신 실패: " + Wire::Wide(pcap_geterr(tx_)));
        return FALSE;
    }

    return TRUE;
}

UINT AFX_CDECL CNILayer::ReadingThreadEntry(LPVOID parameter) {
    return ReadingThread(reinterpret_cast<LPDWORD>(parameter));
}

UINT AFX_CDECL CNILayer::ReadingThread(LPDWORD lpdwParam) {
    auto* layer = reinterpret_cast<CNILayer*>(lpdwParam);
    if (layer == nullptr) {
        return 1;
    }

    layer->ReadLoop();
    return 0;
}

void CNILayer::ReadLoop() {
    // 캡처 이벤트를 기다리면 패킷이 없는 상태에서도 종료 요청을 주기적으로 확인할 수 있다.
    HANDLE captureEvent = pcap_getevent(rx_);

    while (!stop_) {
        CBaseLayer* upperLayer = GetUpperLayer(0);
        if (upperLayer != nullptr) {
            upperLayer->OnIdle();
        }

        if (captureEvent != nullptr) {
            const DWORD waitResult =
                WaitForSingleObject(captureEvent, kReadTimeoutMs);
            if (waitResult != WAIT_TIMEOUT && waitResult != WAIT_OBJECT_0) {
                break;
            }
        }

        // 드라이버를 다시 기다리기 전에 libpcap의 사용자 공간 버퍼를 모두 비운다.
        while (!stop_) {
            pcap_pkthdr* header = nullptr;
            const u_char* frame = nullptr;
            const int readResult = pcap_next_ex(rx_, &header, &frame);

            if (readResult == 0) {
                break;
            }

            if (readResult < 0) {
                if (!stop_) {
                    Status(L"수신이 중단되었습니다. 주소를 재설정하세요.");
                }
                stop_ = true;
                break;
            }

            if (stop_) {
                break;
            }

            // 일부만 캡처된 프레임은 상위 레이어에서 안전하게 해석할 수 없다.
            if (header->caplen != header->len) {
                continue;
            }

            try {
                upperLayer = GetUpperLayer(0);
                if (upperLayer != nullptr) {
                    upperLayer->ReceiveFrame(frame, header->caplen, {});
                }
            } catch (const std::exception&) {
                Status(L"수신 데이터 처리 실패");
            }
        }

        // 일부 pcap 구현은 대기 이벤트를 제공하지 않는다.
        if (captureEvent == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}
