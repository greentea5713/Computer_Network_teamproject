#pragma once
#include "Protocol.h"

#include <pcap.h>

struct NetworkAdapter {
    std::string name;
    std::wstring description;
    MacAddress mac{};
    bool hasMac = false;
    IpAddress ip{};
    bool hasIp = false;
};

class CNILayer : public CBaseLayer {
public:
    explicit CNILayer(const char* name)
        : CBaseLayer(name) {
    }

    ~CNILayer() override {
        Close();
    }

    std::vector<NetworkAdapter> Enumerate();
    bool Open(const NetworkAdapter&);
    void Close();

    BOOL Send(unsigned char*, int) override;

private:
    // 과제 명세의 함수 원형을 유지한다.
    // AfxBeginThread가 요구하는 LPVOID 원형은 ReadingThreadEntry가 변환한다.
    static UINT AFX_CDECL ReadingThread(LPDWORD lpdwParam);
    static UINT AFX_CDECL ReadingThreadEntry(LPVOID parameter);

    void ReadLoop();
    void JoinReader();

    pcap_t* rx_ = nullptr;
    pcap_t* tx_ = nullptr;
    std::atomic<bool> stop_{true};
    CWinThread* reader_ = nullptr;
    std::mutex sendMutex_;
};
