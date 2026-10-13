#include "pch.h"
#include "ProxyAddDlg.h"

BEGIN_MESSAGE_MAP(CProxyAddDlg, CDialogEx)
    ON_CBN_SELCHANGE(IDC_COMBO_PROXY_DEVICE, &CProxyAddDlg::OnDevice)
END_MESSAGE_MAP()

CProxyAddDlg::CProxyAddDlg(
    const std::vector<NetworkAdapter>& adapters,
    int selectedAdapter,
    CWnd* parent)
    : CDialogEx(IDD, parent),
      adapters_(adapters),
      selectedAdapter_(selectedAdapter) {
}

BOOL CProxyAddDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();

    auto* deviceCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_PROXY_DEVICE));
    for (const NetworkAdapter& adapter : adapters_) {
        deviceCombo->AddString(adapter.description.c_str());
    }
    if (selectedAdapter_ >= 0 && selectedAdapter_ < static_cast<int>(adapters_.size())) {
        deviceCombo->SetCurSel(selectedAdapter_);
        OnDevice();
    }
    return TRUE;
}

// 장치를 고르면 그 장치의 MAC을 응답에 쓸 Ethernet 주소로 채운다. (직접 수정 가능)
void CProxyAddDlg::OnDevice() {
    auto* deviceCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_PROXY_DEVICE));
    const int selected = deviceCombo->GetCurSel();
    if (selected >= 0 && selected < static_cast<int>(adapters_.size()) && adapters_[selected].hasMac) {
        SetDlgItemText(IDC_EDIT_PROXY_MAC, Wire::MacText(adapters_[selected].mac).c_str());
    }
}

void CProxyAddDlg::OnOK() {
    CString device;
    GetDlgItemText(IDC_COMBO_PROXY_DEVICE, device);

    auto* ipControl = static_cast<CIPAddressCtrl*>(GetDlgItem(IDC_IP_PROXY));
    BYTE bytes[4]{};
    const bool validIp =
        !ipControl->IsBlank() &&
        ipControl->GetAddress(bytes[0], bytes[1], bytes[2], bytes[3]) == 4 &&
        bytes[0] != 0 && bytes[0] < 224;

    CString macText;
    GetDlgItemText(IDC_EDIT_PROXY_MAC, macText);
    MacAddress mac{};
    const bool validMac = Wire::ParseMac(macText.GetString(), mac) && (mac[0] & 1) == 0;

    if (device.IsEmpty() || !validIp || !validMac) {
        AfxMessageBox(L"Device, 유니캐스트 IP 주소, Ethernet 주소(예: 00:11:22:33:44:55)를 입력하세요.");
        return;
    }

    entry_.device = device.GetString();
    std::copy(std::begin(bytes), std::end(bytes), entry_.ip.begin());
    entry_.mac = mac;
    CDialogEx::OnOK();
}
