#pragma once

#include "resource.h"
#include "LayerManager.h"
#include "IPLayer.h"
#include "NILayer.h"

class CMACFileTransferDlg : public CDialogEx, public CBaseLayer {
public:
    explicit CMACFileTransferDlg(CWnd* parent = nullptr);

    enum {
        IDD = IDD_MACFILETRANSFER_DIALOG
    };

protected:
    BOOL OnInitDialog() override;
    BOOL PreTranslateMessage(MSG*) override;
    void OnCancel() override;
    void OnOK() override {}

    afx_msg void OnSelect();
    afx_msg void OnAdapter();
    afx_msg void OnArpSend();
    afx_msg void OnArpDelete();
    afx_msg void OnArpClear();
    afx_msg void OnTimer(UINT_PTR);
    afx_msg LRESULT OnEvents(WPARAM, LPARAM);

    DECLARE_MESSAGE_MAP()

private:
    void Queue(const std::wstring&);
    void QueueCacheRefresh();
    void RefreshCache();
    bool ReadIp(int control, IpAddress&);
    bool SelectedIp(IpAddress&);
    void Ready(bool);
    void Disconnect();

    CLayerManager manager_;
    CNILayer ni_{"NI"};
    CEthernetLayer ethernet_{"Ethernet"};
    CARPLayer arp_{"ARP"};
    CIPLayer ip_{"IP"};

    std::vector<NetworkAdapter> adapters_;
    bool ready_ = false;

    // 수신 스레드는 UI 컨트롤을 직접 건드리지 않고 이 큐를 통해 결과를 전달한다.
    std::mutex eventMutex_;
    std::deque<std::wstring> events_;
    bool cachePending_ = false;
    bool closing_ = false;
};
