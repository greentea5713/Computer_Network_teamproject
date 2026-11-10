#include "pch.h"
#include "ARP.h"
#include "ARPDlg.h"
#include "ProxyAddDlg.h"

namespace {
constexpr UINT WM_NETWORK_EVENT = WM_APP + 17;
constexpr size_t kMaximumQueuedEvents = 1000;
constexpr UINT_PTR kCacheTimer = 1;
constexpr UINT kCacheTimerMs = 1000;
constexpr wchar_t kTitle[] = L"Router";

enum CacheColumn {
    kCacheIp,
    kCacheMac,
    kCacheState,
    kCachePort,
};

enum ProxyColumn {
    kProxyDevice,
    kProxyIp,
    kProxyMac,
};
}  // namespace

BEGIN_MESSAGE_MAP(CARPDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BUTTON_SELECT, &CARPDlg::OnSelect)
    ON_BN_CLICKED(IDC_ROUTE_ADD, &CARPDlg::OnRouteAdd)
    ON_BN_CLICKED(IDC_ROUTE_DELETE, &CARPDlg::OnRouteDelete)
    ON_BN_CLICKED(IDC_BUTTON_ARP_SEND, &CARPDlg::OnArpSend)
    ON_BN_CLICKED(IDC_BUTTON_ARP_DELETE, &CARPDlg::OnArpDelete)
    ON_BN_CLICKED(IDC_BUTTON_ARP_CLEAR, &CARPDlg::OnArpClear)
    ON_BN_CLICKED(IDC_BUTTON_PROXY_ADD, &CARPDlg::OnProxyAdd)
    ON_BN_CLICKED(IDC_BUTTON_PROXY_DELETE, &CARPDlg::OnProxyDelete)
    ON_BN_CLICKED(IDC_BUTTON_GARP_SEND, &CARPDlg::OnGarpSend)
    ON_CBN_SELCHANGE(IDC_COMBO_ADAPTER, &CARPDlg::OnAdapter)
    ON_CBN_SELCHANGE(IDC_COMBO_ADAPTER2, &CARPDlg::OnAdapter2)
    ON_WM_TIMER()
    ON_MESSAGE(WM_NETWORK_EVENT, &CARPDlg::OnEvents)
END_MESSAGE_MAP()

CARPDlg::CARPDlg(CWnd* parent)
    : CDialogEx(IDD, parent),
      CBaseLayer("ARPDlg") {
    manager_.AddLayer(&ni_);
    manager_.AddLayer(&ethernet_);
    manager_.AddLayer(&arp_);
    manager_.AddLayer(&ip_);
    manager_.AddLayer(&input1_);
    manager_.AddLayer(this);

    // NI → Ethernet에서 EtherType으로 분기한다. 0x0806 ARP, 0x0800 IP
    // ARP와 IP는 Ethernet 위에 나란히 놓이고, Application(Dialog)은 IP 위에 놓인다.
    manager_.ConnectLayers("NI ( *Ethernet ( *ARP *Router1 ) )");
    ni2_.SetUpperUnderLayer(&ethernet2_);
    ethernet2_.SetUpperUnderLayer(&arp2_);
    ethernet2_.SetUpperUnderLayer(&input2_);
    router_.ports[0].ethernet = &ethernet_;
    router_.ports[0].arp = &arp_;
    router_.ports[1].ethernet = &ethernet2_;
    router_.ports[1].arp = &arp2_;
    // IP 레이어는 목적지 Ethernet 주소 해석을 ARP 레이어에 요청한다.
    ip_.SetArpLayer(&arp_);

    const auto notify = [this](const std::wstring& message) {
        Queue(message);
    };
    ni_.notify = notify;
    arp_.notify = notify;
    arp_.onCacheChanged = [this] { QueueCacheRefresh(); };
    arp_.onDuplicateIp = [this](const MacAddress& mac) { QueueDuplicate(mac); };
    ni2_.notify = notify;
    arp2_.notify = notify;
    arp2_.onCacheChanged = [this] { QueueCacheRefresh(); };
}

BOOL CARPDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetIcon(AfxGetApp()->LoadIcon(IDR_MAINFRAME), TRUE);
    SetWindowText(kTitle);

    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    cacheList->SetExtendedStyle(LVS_EX_FULLROWSELECT);
    cacheList->InsertColumn(kCacheIp, L"IP Address", LVCFMT_LEFT, 130);
    cacheList->InsertColumn(kCacheMac, L"Ethernet Address", LVCFMT_LEFT, 150);
    cacheList->InsertColumn(kCacheState, L"Status", LVCFMT_LEFT, 80);
    cacheList->InsertColumn(kCachePort, L"Port", LVCFMT_LEFT, 55);

    auto* proxyList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_PROXY));
    proxyList->SetExtendedStyle(LVS_EX_FULLROWSELECT);
    proxyList->InsertColumn(kProxyDevice, L"Device", LVCFMT_LEFT, 100);
    proxyList->InsertColumn(kProxyIp, L"IP Address", LVCFMT_LEFT, 120);
    proxyList->InsertColumn(kProxyMac, L"Ethernet Address", LVCFMT_LEFT, 140);

    SetTimer(kCacheTimer, kCacheTimerMs, nullptr);

    adapters_ = ni_.Enumerate();
    auto* adapterCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER));
    auto* adapter2 = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER2));
    for (const NetworkAdapter& adapter : adapters_) {
        adapterCombo->AddString(adapter.description.c_str());
        adapter2->AddString(adapter.description.c_str());
    }
    if (adapters_.size() > 1) { adapter2->SetCurSel(1); OnAdapter2(); }
    auto* port = static_cast<CComboBox*>(GetDlgItem(IDC_ROUTE_PORT));
    port->AddString(L"0"); port->AddString(L"1"); port->SetCurSel(0);
    auto* flag = static_cast<CComboBox*>(GetDlgItem(IDC_ROUTE_FLAG));
    flag->AddString(L"U"); flag->AddString(L"UG"); flag->SetCurSel(0);
    auto* routes = static_cast<CListCtrl*>(GetDlgItem(IDC_ROUTE_LIST));
    routes->SetExtendedStyle(LVS_EX_FULLROWSELECT);
    routes->InsertColumn(0, L"Destination", LVCFMT_LEFT, 125);
    routes->InsertColumn(1, L"Mask", LVCFMT_LEFT, 125);
    routes->InsertColumn(2, L"Gateway", LVCFMT_LEFT, 125);
    routes->InsertColumn(3, L"Port", LVCFMT_LEFT, 55);
    routes->InsertColumn(4, L"Flag", LVCFMT_LEFT, 55);

    if (!adapters_.empty()) {
        adapterCombo->SetCurSel(0);
        OnAdapter();
    } else {
        AfxMessageBox(L"어댑터가 없습니다. Npcap 설치와 권한을 확인하세요.");
    }

    Ready(false);
    return TRUE;
}

void CARPDlg::Ready(bool state) {
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
    GetDlgItem(IDC_COMBO_ADAPTER2)->EnableWindow(!state);
    GetDlgItem(IDC_IP_SRC2)->EnableWindow(!state);

    SetDlgItemText(IDC_BUTTON_SELECT, state ? L"Reset" : L"Select");
}

bool CARPDlg::ReadIp(int control, IpAddress& address) {
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

bool CARPDlg::SelectedIp(IpAddress& address) {
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    return selected >= 0 &&
        Wire::ParseIp(cacheList->GetItemText(selected, kCacheIp).GetString(), address);
}

void CARPDlg::OnAdapter() {
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

void CARPDlg::OnAdapter2() {
    const int index = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER2))->GetCurSel();
    if (index < 0 || index >= static_cast<int>(adapters_.size())) return;
    const auto& adapter = adapters_[index];
    SetDlgItemText(IDC_EDIT_SRC2, adapter.hasMac ? Wire::MacText(adapter.mac).c_str() : L"");
    auto* control = static_cast<CIPAddressCtrl*>(GetDlgItem(IDC_IP_SRC2));
    if (adapter.hasIp) control->SetAddress(adapter.ip[0], adapter.ip[1], adapter.ip[2], adapter.ip[3]);
    else control->ClearAddress();
}

void CARPDlg::OnSelect() {
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

    const int second = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER2))->GetCurSel();
    IpAddress secondIp{};
    if (second < 0 || second >= static_cast<int>(adapters_.size()) || second == selectedIndex ||
        !adapters_[second].hasMac || !ReadIp(IDC_IP_SRC2, secondIp) || secondIp[0] == 0 || secondIp[0] >= 224 || secondIp == myIp) {
        AfxMessageBox(L"Select two different wired adapters and enter a valid Port 1 IP.");
        return;
    }
    ethernet_.SetSourceAddress(source);
    ip_.SetSourceAddress(myIp);
    arp_.SetSourceAddress(source, myIp);
    ethernet2_.SetSourceAddress(adapters_[second].mac);
    arp2_.SetSourceAddress(adapters_[second].mac, secondIp);
    router_.ports[0].ip = myIp;
    router_.ports[1].ip = secondIp;

    if (ni_.Open(adapter) && ni2_.Open(adapters_[second])) {
        Ready(true);
        SetDlgItemText(IDC_EDIT_GARP, Wire::MacText(source).c_str());
        Queue(L"주소 설정 완료: " + Wire::IpText(myIp) + L" / " + Wire::MacText(source));
    } else {
        ni_.Close();
        ni2_.Close();
        AfxMessageBox(L"장치를 열지 못했습니다. Npcap 설정을 확인하세요.");
    }
}

void CARPDlg::OnArpSend() {
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

void CARPDlg::OnArpDelete() {
    IpAddress target{};
    if (!SelectedIp(target)) {
        AfxMessageBox(L"삭제할 ARP 캐시 항목을 선택하세요.");
        return;
    }

    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int row = cacheList->GetNextItem(-1, LVNI_SELECTED);
    CARPLayer& cache = cacheList->GetItemText(row, kCachePort) == L"1" ? arp2_ : arp_;
    if (cache.Remove(target)) {
        Queue(L"ARP 캐시 항목 삭제: " + Wire::IpText(target));
    }
}

void CARPDlg::OnArpClear() {
    arp_.Clear();
    arp2_.Clear();
    Queue(L"ARP 캐시 전체 삭제");
}

void CARPDlg::OnProxyAdd() {
    auto* adapterCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER));
    CProxyAddDlg dialog(adapters_, adapterCombo->GetCurSel(), this);
    if (dialog.DoModal() != IDOK) {
        return;
    }

    if (!arp_.AddProxy(dialog.Entry())) {
        AfxMessageBox(L"내 IP 주소는 Proxy ARP 항목으로 추가할 수 없습니다.");
        return;
    }
    RefreshProxy();
    Queue(L"Proxy ARP 항목 추가: " + Wire::IpText(dialog.Entry().ip));
}

void CARPDlg::OnProxyDelete() {
    auto* proxyList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_PROXY));
    const int selected = proxyList->GetNextItem(-1, LVNI_SELECTED);
    IpAddress target{};
    if (selected < 0 ||
        !Wire::ParseIp(proxyList->GetItemText(selected, kProxyIp).GetString(), target)) {
        AfxMessageBox(L"삭제할 Proxy ARP 항목을 선택하세요.");
        return;
    }

    if (arp_.RemoveProxy(target)) {
        RefreshProxy();
        Queue(L"Proxy ARP 항목 삭제: " + Wire::IpText(target));
    }
}

void CARPDlg::OnGarpSend() {
    if (!ready_) {
        return;
    }

    CString macText;
    GetDlgItemText(IDC_EDIT_GARP, macText);
    MacAddress mac{};
    if (!Wire::ParseMac(macText.GetString(), mac) || (mac[0] & 1) != 0) {
        AfxMessageBox(L"알릴 유니캐스트 H/W 주소를 입력하세요. 예: 00:11:22:33:44:55");
        return;
    }

    // 입력한 H/W 주소를 내 MAC으로 바꾼 뒤(NIC 교체 상황) 자기 IP에 대한 ARP 요청을 브로드캐스트한다.
    ethernet_.SetSourceAddress(mac);
    arp_.SetSourceAddress(mac, ip_.GetSourceAddress());
    arp_.SendGratuitous();
}

void CARPDlg::RefreshProxy() {
    auto* proxyList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_PROXY));
    const std::vector<CARPLayer::ProxyEntry> entries = arp_.ProxySnapshot();

    proxyList->DeleteAllItems();
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const CARPLayer::ProxyEntry& entry = entries[index];
        proxyList->InsertItem(index, entry.device.c_str());
        proxyList->SetItemText(index, kProxyIp, Wire::IpText(entry.ip).c_str());
        proxyList->SetItemText(index, kProxyMac, Wire::MacText(entry.mac).c_str());
    }
}

void CARPDlg::OnTimer(UINT_PTR id) {
    if (id != kCacheTimer) {
        CDialogEx::OnTimer(id);
        return;
    }

    // 수신 스레드가 멈춘 상태에서도 만료 항목을 정리한다.
    arp_.Expire(GetTickCount64());
    arp2_.Expire(GetTickCount64());
    router_.Flush();
}

void CARPDlg::OnRouteAdd() {
    RouteEntry entry;
    auto* flag = static_cast<CComboBox*>(GetDlgItem(IDC_ROUTE_FLAG));
    entry.viaGateway = flag->GetCurSel() == 1;
    entry.port = static_cast<CComboBox*>(GetDlgItem(IDC_ROUTE_PORT))->GetCurSel();
    if (!ReadIp(IDC_ROUTE_DEST, entry.destination) || !ReadIp(IDC_ROUTE_MASK, entry.mask) ||
        (entry.viaGateway && !ReadIp(IDC_ROUTE_GW, entry.gateway)) || !RouterCore::ValidRoute(entry)) {
        AfxMessageBox(L"Check destination, contiguous netmask, gateway and interface.");
        return;
    }
    routes_.push_back(entry);
    router_.SetRoutes(routes_);
    auto* list = static_cast<CListCtrl*>(GetDlgItem(IDC_ROUTE_LIST));
    const int row = list->InsertItem(list->GetItemCount(), Wire::IpText(entry.destination).c_str());
    list->SetItemText(row, 1, Wire::IpText(entry.mask).c_str());
    list->SetItemText(row, 2, entry.viaGateway ? Wire::IpText(entry.gateway).c_str() : L"-");
    list->SetItemText(row, 3, entry.port == 0 ? L"Port 0" : L"Port 1");
    list->SetItemText(row, 4, entry.viaGateway ? L"UG" : L"U");
}

void CARPDlg::OnRouteDelete() {
    auto* list = static_cast<CListCtrl*>(GetDlgItem(IDC_ROUTE_LIST));
    const int row = list->GetNextItem(-1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(routes_.size())) return;
    routes_.erase(routes_.begin() + row);
    list->DeleteItem(row);
    router_.SetRoutes(routes_);
}

void CARPDlg::RefreshCache() {
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    const CString selectedIp =
        selected >= 0 ? cacheList->GetItemText(selected, kCacheIp) : CString();

    auto entries = arp_.Snapshot();
    const size_t port0Count = entries.size();
    const auto port1Entries = arp2_.Snapshot();
    entries.insert(entries.end(), port1Entries.begin(), port1Entries.end());

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
        cacheList->SetItemText(index, kCachePort, static_cast<size_t>(index) < port0Count ? L"0" : L"1");

        if (selectedIp == ip.c_str()) {
            cacheList->SetItemState(index, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    cacheList->SetRedraw(TRUE);
    cacheList->Invalidate();
}

void CARPDlg::Queue(const std::wstring& message) {
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

void CARPDlg::QueueCacheRefresh() {
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

void CARPDlg::QueueDuplicate(const MacAddress& mac) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (closing_) {
        return;
    }

    const bool shouldWakeUi = events_.empty() && !cachePending_ && !duplicatePending_;
    duplicateMac_ = mac;
    duplicatePending_ = true;
    if (shouldWakeUi && GetSafeHwnd() != nullptr) {
        PostMessage(WM_NETWORK_EVENT);
    }
}

LRESULT CARPDlg::OnEvents(WPARAM, LPARAM) {
    std::deque<std::wstring> batch;
    bool cachePending = false;
    bool duplicatePending = false;
    MacAddress duplicateMac{};
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        batch.swap(events_);
        cachePending = cachePending_;
        cachePending_ = false;
        duplicatePending = duplicatePending_;
        duplicatePending_ = false;
        duplicateMac = duplicateMac_;
    }

    if (cachePending) {
        RefreshCache();
    }

    // 디자인에 로그 영역이 없으므로 가장 최근 상태를 제목 표시줄에 보여 준다.
    if (!batch.empty()) {
        SetWindowText((std::wstring(kTitle) + L" - " + batch.back()).c_str());
    }

    // IP 중복은 놓치지 않도록 메시지 상자로 알린다. 상자가 떠 있는 동안 다시 띄우지 않는다.
    if (duplicatePending && !duplicateShowing_) {
        duplicateShowing_ = true;
        MessageBox(
            (L"duplicate IP address sent from Ethernet address: " + Wire::MacText(duplicateMac)).c_str(),
            kTitle,
            MB_ICONWARNING | MB_OK);
        duplicateShowing_ = false;
    }
    return 0;
}

void CARPDlg::Disconnect() {
    ni_.Close();
    ni2_.Close();
    router_.ClearPending();
    arp_.Clear();
    arp2_.Clear();
    Ready(false);
    SetWindowText(kTitle);
}

void CARPDlg::OnCancel() {
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        closing_ = true;
    }

    KillTimer(kCacheTimer);
    Disconnect();
    CDialogEx::OnCancel();
}

BOOL CARPDlg::PreTranslateMessage(MSG* message) {
    if (message->message == WM_KEYDOWN && message->wParam == VK_RETURN) {
        // IP 주소 컨트롤은 내부 편집 상자가 포커스를 가지므로 부모 ID를 확인한다.
        if (::GetDlgCtrlID(::GetParent(::GetFocus())) == IDC_IP_TARGET) {
            OnArpSend();
        }
        return TRUE;
    }

    return CDialogEx::PreTranslateMessage(message);
}
