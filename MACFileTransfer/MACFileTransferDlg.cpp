#include "pch.h"
#include "MACFileTransfer.h"
#include "MACFileTransferDlg.h"

namespace {
constexpr UINT WM_NETWORK_EVENT = WM_APP + 17;
constexpr size_t kMaximumQueuedEvents = 1000;
constexpr UINT_PTR kCacheTimer = 1;
constexpr UINT kCacheTimerMs = 1000;
constexpr wchar_t kTitle[] = L"ARP";

enum CacheColumn {
    kCacheIp,
    kCacheMac,
    kCacheState,
};

enum ProxyColumn {
    kProxyDevice,
    kProxyIp,
    kProxyMac,
};
}  // namespace

BEGIN_MESSAGE_MAP(CMACFileTransferDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BUTTON_SELECT, &CMACFileTransferDlg::OnSelect)
    ON_BN_CLICKED(IDC_BUTTON_ARP_SEND, &CMACFileTransferDlg::OnArpSend)
    ON_BN_CLICKED(IDC_BUTTON_ARP_DELETE, &CMACFileTransferDlg::OnArpDelete)
    ON_BN_CLICKED(IDC_BUTTON_ARP_CLEAR, &CMACFileTransferDlg::OnArpClear)
    ON_CBN_SELCHANGE(IDC_COMBO_ADAPTER, &CMACFileTransferDlg::OnAdapter)
    ON_WM_TIMER()
    ON_MESSAGE(WM_NETWORK_EVENT, &CMACFileTransferDlg::OnEvents)
END_MESSAGE_MAP()

CMACFileTransferDlg::CMACFileTransferDlg(CWnd* parent)
    : CDialogEx(IDD, parent),
      CBaseLayer("ARPDlg") {
    manager_.AddLayer(&ni_);
    manager_.AddLayer(&ethernet_);
    manager_.AddLayer(&arp_);
    manager_.AddLayer(&ip_);
    manager_.AddLayer(this);

    // NI → Ethernet에서 EtherType으로 분기한다. 0x0806 ARP, 0x0800 IP
    // ARP와 IP는 Ethernet 위에 나란히 놓이고, Application(Dialog)은 IP 위에 놓인다.
    manager_.ConnectLayers("NI ( *Ethernet ( *ARP *IP ( +ARPDlg ) ) )");
    // IP 레이어는 목적지 Ethernet 주소 해석을 ARP 레이어에 요청한다.
    ip_.SetArpLayer(&arp_);

    const auto notify = [this](const std::wstring& message) {
        Queue(message);
    };
    ni_.notify = notify;
    arp_.notify = notify;
    arp_.onCacheChanged = [this] { QueueCacheRefresh(); };
}

BOOL CMACFileTransferDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetIcon(AfxGetApp()->LoadIcon(IDR_MAINFRAME), TRUE);
    SetWindowText(kTitle);

    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    cacheList->SetExtendedStyle(LVS_EX_FULLROWSELECT);
    cacheList->InsertColumn(kCacheIp, L"IP Address", LVCFMT_LEFT, 130);
    cacheList->InsertColumn(kCacheMac, L"Ethernet Address", LVCFMT_LEFT, 150);
    cacheList->InsertColumn(kCacheState, L"Status", LVCFMT_LEFT, 80);

    auto* proxyList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_PROXY));
    proxyList->SetExtendedStyle(LVS_EX_FULLROWSELECT);
    proxyList->InsertColumn(kProxyDevice, L"Device", LVCFMT_LEFT, 100);
    proxyList->InsertColumn(kProxyIp, L"IP Address", LVCFMT_LEFT, 120);
    proxyList->InsertColumn(kProxyMac, L"Ethernet Address", LVCFMT_LEFT, 140);

    SetTimer(kCacheTimer, kCacheTimerMs, nullptr);

    adapters_ = ni_.Enumerate();
    auto* adapterCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER));
    for (const NetworkAdapter& adapter : adapters_) {
        adapterCombo->AddString(adapter.description.c_str());
    }

    if (!adapters_.empty()) {
        adapterCombo->SetCurSel(0);
        OnAdapter();
    } else {
        AfxMessageBox(L"어댑터가 없습니다. Npcap 설치와 권한을 확인하세요.");
    }

    Ready(false);
    return TRUE;
}

void CMACFileTransferDlg::Ready(bool state) {
    ready_ = state;

    for (int control : {
             IDC_BUTTON_ARP_SEND,
             IDC_BUTTON_ARP_DELETE,
             IDC_BUTTON_ARP_CLEAR,
             IDC_IP_TARGET,
             IDC_BUTTON_PROXY_ADD,
             IDC_BUTTON_PROXY_DELETE,
             IDC_EDIT_GARP,
             IDC_BUTTON_GARP_SEND}) {
        GetDlgItem(control)->EnableWindow(state);
    }

    GetDlgItem(IDC_COMBO_ADAPTER)->EnableWindow(!state);
    GetDlgItem(IDC_EDIT_SRC)->EnableWindow(!state);
    GetDlgItem(IDC_IP_SRC)->EnableWindow(!state);

    SetDlgItemText(IDC_BUTTON_SELECT, state ? L"Reset" : L"Select");
}

bool CMACFileTransferDlg::ReadIp(int control, IpAddress& address) {
    auto* ipControl = static_cast<CIPAddressCtrl*>(GetDlgItem(control));
    if (ipControl->IsBlank()) {
        return false;
    }

    BYTE bytes[4]{};
    if (ipControl->GetAddress(bytes[0], bytes[1], bytes[2], bytes[3]) != 4) {
        return false;
    }
    std::copy(std::begin(bytes), std::end(bytes), address.begin());
    return true;
}

bool CMACFileTransferDlg::SelectedIp(IpAddress& address) {
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    return selected >= 0 &&
        Wire::ParseIp(cacheList->GetItemText(selected, kCacheIp).GetString(), address);
}

void CMACFileTransferDlg::OnAdapter() {
    auto* adapterCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER));
    const int selectedIndex = adapterCombo->GetCurSel();
    if (selectedIndex < 0 ||
        selectedIndex >= static_cast<int>(adapters_.size())) {
        return;
    }

    const NetworkAdapter& adapter = adapters_[selectedIndex];
    SetDlgItemText(
        IDC_EDIT_SRC,
        adapter.hasMac ? Wire::MacText(adapter.mac).c_str() : L"");

    auto* ipControl = static_cast<CIPAddressCtrl*>(GetDlgItem(IDC_IP_SRC));
    if (adapter.hasIp) {
        ipControl->SetAddress(adapter.ip[0], adapter.ip[1], adapter.ip[2], adapter.ip[3]);
    } else {
        ipControl->ClearAddress();
    }
}

void CMACFileTransferDlg::OnSelect() {
    if (ready_) {
        Disconnect();
        return;
    }

    auto* adapterCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER));
    const int selectedIndex = adapterCombo->GetCurSel();
    if (selectedIndex < 0 ||
        selectedIndex >= static_cast<int>(adapters_.size())) {
        AfxMessageBox(L"어댑터를 선택하세요.");
        return;
    }

    CString sourceText;
    GetDlgItemText(IDC_EDIT_SRC, sourceText);

    MacAddress source{};
    const bool validSource =
        Wire::ParseMac(sourceText.GetString(), source) && (source[0] & 1) == 0;
    if (!validSource) {
        AfxMessageBox(L"출발지 유니캐스트 MAC 주소를 입력하세요. 예: 00:11:22:33:44:55");
        return;
    }

    const NetworkAdapter& adapter = adapters_[selectedIndex];
    if (adapter.hasMac && adapter.mac != source) {
        AfxMessageBox(L"출발지 주소는 선택한 NIC의 실제 MAC과 같아야 합니다.");
        return;
    }

    IpAddress myIp{};
    if (!ReadIp(IDC_IP_SRC, myIp) || myIp[0] == 0 || myIp[0] >= 224) {
        AfxMessageBox(L"내 유니캐스트 IP 주소를 입력하세요. 예: 168.188.129.2");
        return;
    }

    ethernet_.SetSourceAddress(source);
    ip_.SetSourceAddress(myIp);
    arp_.SetSourceAddress(source, myIp);

    if (ni_.Open(adapter)) {
        Ready(true);
        Queue(L"주소 설정 완료: " + Wire::IpText(myIp) + L" / " + Wire::MacText(source));
    } else {
        AfxMessageBox(L"장치를 열지 못했습니다. Npcap 설정을 확인하세요.");
    }
}

void CMACFileTransferDlg::OnArpSend() {
    if (!ready_) {
        return;
    }

    IpAddress target{};
    if (!ReadIp(IDC_IP_TARGET, target) || target[0] == 0 || target[0] >= 224) {
        AfxMessageBox(L"ARP 요청을 보낼 대상 IP 주소를 입력하세요.");
        return;
    }
    if (target == ip_.GetSourceAddress()) {
        AfxMessageBox(L"내 IP 주소와 다른 대상 IP 주소를 입력하세요.");
        return;
    }

    // Application → IP → ARP 순서로 주소 해석을 요청한다.
    ip_.Request(target);
}

void CMACFileTransferDlg::OnArpDelete() {
    IpAddress target{};
    if (!SelectedIp(target)) {
        AfxMessageBox(L"삭제할 ARP 캐시 항목을 선택하세요.");
        return;
    }

    if (arp_.Remove(target)) {
        Queue(L"ARP 캐시 항목 삭제: " + Wire::IpText(target));
    }
}

void CMACFileTransferDlg::OnArpClear() {
    arp_.Clear();
    Queue(L"ARP 캐시 전체 삭제");
}

void CMACFileTransferDlg::OnTimer(UINT_PTR id) {
    if (id != kCacheTimer) {
        CDialogEx::OnTimer(id);
        return;
    }

    // 수신 스레드가 멈춘 상태에서도 만료 항목을 정리한다.
    arp_.Expire(GetTickCount64());
}

void CMACFileTransferDlg::RefreshCache() {
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    const CString selectedIp =
        selected >= 0 ? cacheList->GetItemText(selected, kCacheIp) : CString();

    const std::vector<CARPLayer::CacheEntry> entries = arp_.Snapshot();

    cacheList->SetRedraw(FALSE);
    cacheList->DeleteAllItems();
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const CARPLayer::CacheEntry& entry = entries[index];
        const bool complete = entry.state == CARPLayer::EntryState::Complete;

        const std::wstring ip = Wire::IpText(entry.ip);
        cacheList->InsertItem(index, ip.c_str());
        cacheList->SetItemText(
            index,
            kCacheMac,
            complete ? Wire::MacText(entry.mac).c_str() : L"???????");
        cacheList->SetItemText(index, kCacheState, complete ? L"complete" : L"incomplete");

        if (selectedIp == ip.c_str()) {
            cacheList->SetItemState(index, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    cacheList->SetRedraw(TRUE);
    cacheList->Invalidate();
}

void CMACFileTransferDlg::Queue(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (closing_) {
        return;
    }

    if (events_.size() >= kMaximumQueuedEvents) {
        events_.pop_front();
    }

    const bool shouldWakeUi = events_.empty() && !cachePending_;
    events_.push_back(message);
    if (shouldWakeUi && GetSafeHwnd() != nullptr) {
        PostMessage(WM_NETWORK_EVENT);
    }
}

void CMACFileTransferDlg::QueueCacheRefresh() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (closing_) {
        return;
    }

    const bool shouldWakeUi = events_.empty() && !cachePending_;
    cachePending_ = true;
    if (shouldWakeUi && GetSafeHwnd() != nullptr) {
        PostMessage(WM_NETWORK_EVENT);
    }
}

LRESULT CMACFileTransferDlg::OnEvents(WPARAM, LPARAM) {
    std::deque<std::wstring> batch;
    bool cachePending = false;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        batch.swap(events_);
        cachePending = cachePending_;
        cachePending_ = false;
    }

    if (cachePending) {
        RefreshCache();
    }

    // 디자인에 로그 영역이 없으므로 가장 최근 상태를 제목 표시줄에 보여 준다.
    if (!batch.empty()) {
        SetWindowText((std::wstring(kTitle) + L" - " + batch.back()).c_str());
    }
    return 0;
}

void CMACFileTransferDlg::Disconnect() {
    ni_.Close();
    Ready(false);
    SetWindowText(kTitle);
}

void CMACFileTransferDlg::OnCancel() {
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        closing_ = true;
    }

    KillTimer(kCacheTimer);
    Disconnect();
    CDialogEx::OnCancel();
}

BOOL CMACFileTransferDlg::PreTranslateMessage(MSG* message) {
    if (message->message == WM_KEYDOWN && message->wParam == VK_RETURN) {
        // IP 주소 컨트롤은 내부 편집 상자가 포커스를 가지므로 부모 ID를 확인한다.
        if (::GetDlgCtrlID(::GetParent(::GetFocus())) == IDC_IP_TARGET) {
            OnArpSend();
        }
        return TRUE;
    }

    return CDialogEx::PreTranslateMessage(message);
}
