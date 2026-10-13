#pragma once

#include "resource.h"
#include "ARPLayer.h"
#include "NILayer.h"

// Proxy ARP 테이블 항목(Device, IP Address, Ethernet Address)을 입력받는 대화 상자
class CProxyAddDlg : public CDialogEx {
public:
    CProxyAddDlg(const std::vector<NetworkAdapter>& adapters, int selectedAdapter, CWnd* parent = nullptr);

    enum {
        IDD = IDD_PROXY_ADD
    };

    const CARPLayer::ProxyEntry& Entry() const { return entry_; }

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

    afx_msg void OnDevice();

    DECLARE_MESSAGE_MAP()

private:
    const std::vector<NetworkAdapter>& adapters_;
    int selectedAdapter_ = -1;
    CARPLayer::ProxyEntry entry_;
};
