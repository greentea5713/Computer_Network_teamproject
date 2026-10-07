#pragma once

#include "resource.h"
#include "LayerManager.h"
#include "ChatAppLayer.h"
#include "FileAppLayer.h"
#include "IPLayer.h"
#include "NILayer.h"

class CMACFileTransferDlg : public CDialogEx, public CBaseLayer {
public:
    explicit CMACFileTransferDlg(CWnd* parent = nullptr);

    enum {
        IDD = IDD_MACFILETRANSFER_DIALOG
    };

    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG*) override;
    void OnCancel() override;
    void OnOK() override {}

    afx_msg void OnAddress();
    afx_msg void OnSend();
    afx_msg void OnBrowse();
    afx_msg void OnFileSend();
    afx_msg void OnAdapter();
    afx_msg void OnArpSend();
    afx_msg void OnArpDelete();
    afx_msg void OnArpClear();
    afx_msg void OnArpDoubleClick(NMHDR*, LRESULT*);
    afx_msg void OnTimer(UINT_PTR);
    afx_msg LRESULT OnEvents(WPARAM, LPARAM);

    DECLARE_MESSAGE_MAP()

private:
    void Queue(const std::wstring&);
    void QueueProgress(const CFileAppLayer::Progress&);
    void QueueCacheRefresh();
    void ShowProgress(const CFileAppLayer::Progress&);
    void RefreshCache();
    bool ApplyDestination();
    bool ReadIp(int control, IpAddress&);
    void Ready(bool);
    void Disconnect();

    CLayerManager manager_;
    CNILayer ni_{"NI"};
    CEthernetLayer ethernet_{"Ethernet"};
    CChatAppLayer chat_{"ChatApp"};
    CFileAppLayer file_{"FileApp"};
    CARPLayer arp_{"ARP"};
    CIPLayer ip_{"IP"};

    std::vector<NetworkAdapter> adapters_;
    bool ready_ = false;

    // 작업 스레드는 UI 컨트롤을 직접 건드리지 않고 이 큐를 통해 결과를 전달한다.
    std::mutex eventMutex_;
    std::deque<std::wstring> events_;
    std::array<CFileAppLayer::Progress, 2> progress_{};
    std::array<bool, 2> progressPending_{};
    bool cachePending_ = false;
    bool closing_ = false;

    std::wstring filePath_;
};
