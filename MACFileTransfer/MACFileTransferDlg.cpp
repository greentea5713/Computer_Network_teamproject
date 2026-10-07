#include "pch.h"
#include "MACFileTransfer.h"
#include "MACFileTransferDlg.h"

namespace {
constexpr UINT WM_NETWORK_EVENT = WM_APP + 17;
constexpr size_t kMaximumQueuedEvents = 1000;
constexpr size_t kMaximumListItems = 1000;
constexpr size_t kListItemCharacters = 2000;
constexpr size_t kStatusCharacters = 180;
constexpr UINT_PTR kCacheTimer = 1;
constexpr UINT kCacheTimerMs = 1000;

enum CacheColumn {
    kColumnIp,
    kColumnMac,
    kColumnState,
    kColumnTime,
};
}  // namespace

BEGIN_MESSAGE_MAP(CMACFileTransferDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BUTTON_ADDR, &CMACFileTransferDlg::OnAddress)
    ON_BN_CLICKED(IDC_BUTTON_SEND, &CMACFileTransferDlg::OnSend)
    ON_BN_CLICKED(IDC_BUTTON_BROWSE, &CMACFileTransferDlg::OnBrowse)
    ON_BN_CLICKED(IDC_BUTTON_FILE_SEND, &CMACFileTransferDlg::OnFileSend)
    ON_BN_CLICKED(IDC_BUTTON_ARP_SEND, &CMACFileTransferDlg::OnArpSend)
    ON_BN_CLICKED(IDC_BUTTON_ARP_DELETE, &CMACFileTransferDlg::OnArpDelete)
    ON_BN_CLICKED(IDC_BUTTON_ARP_CLEAR, &CMACFileTransferDlg::OnArpClear)
    ON_NOTIFY(NM_DBLCLK, IDC_LIST_ARP, &CMACFileTransferDlg::OnArpDoubleClick)
    ON_CBN_SELCHANGE(IDC_COMBO_ADAPTER, &CMACFileTransferDlg::OnAdapter)
    ON_WM_TIMER()
    ON_MESSAGE(WM_NETWORK_EVENT, &CMACFileTransferDlg::OnEvents)
END_MESSAGE_MAP()

CMACFileTransferDlg::CMACFileTransferDlg(CWnd* parent)
    : CDialogEx(IDD, parent),
      CBaseLayer("ChatDlg") {
    manager_.AddLayer(&ni_);
    manager_.AddLayer(&ethernet_);
    manager_.AddLayer(&chat_);
    manager_.AddLayer(&file_);
    manager_.AddLayer(&arp_);
    manager_.AddLayer(&ip_);
    manager_.AddLayer(this);

    // NI → Ethernet에서 EtherType으로 분기한다.
    //   0x2080 ChatApp, 0x2090 FileApp, 0x0806 ARP, 0x0800 IP
    // ARP와 IP는 Ethernet 위에 나란히 놓이고, Application(Dialog)은 IP 위에 놓인다.
    manager_.ConnectLayers(
        "NI ( *Ethernet ( *ChatApp ( +ChatDlg ) *FileApp ( +ChatDlg ) *ARP *IP ( +ChatDlg ) ) )");
    // IP 레이어는 목적지 Ethernet 주소 해석을 ARP 레이어에 요청한다.
    ip_.SetArpLayer(&arp_);

    const auto notify = [this](const std::wstring& message) {
        Queue(message);
    };
    ni_.notify = notify;
    chat_.notify = notify;
    file_.notify = notify;
    arp_.notify = notify;
    file_.onProgress = [this](const CFileAppLayer::Progress& progress) { QueueProgress(progress); };
    arp_.onCacheChanged = [this] { QueueCacheRefresh(); };
}

BOOL CMACFileTransferDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetIcon(AfxGetApp()->LoadIcon(IDR_MAINFRAME), TRUE);
    for (int control : {IDC_PROGRESS_SEND, IDC_PROGRESS_RECEIVE}) {
        GetDlgItem(control)->SendMessage(PBM_SETRANGE32, 0, 100);
        GetDlgItem(control)->SendMessage(PBM_SETPOS, 0);
    }

    // 긴 채팅도 입력 컨트롤 자체의 기본 제한에 막히지 않도록 한다.
    auto* messageEdit = static_cast<CEdit*>(GetDlgItem(IDC_EDIT_MSG));
    messageEdit->SetLimitText(0x7ffffffe);

    wchar_t executablePath[32768]{};
    GetModuleFileNameW(nullptr, executablePath, 32768);
    std::wstring receiveDirectory = executablePath;
    receiveDirectory =
        receiveDirectory.substr(0, receiveDirectory.find_last_of(L'\\')) +
        L"\\Received";
    file_.SetDirectory(receiveDirectory);

    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    cacheList->SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    cacheList->InsertColumn(kColumnIp, L"IP 주소", LVCFMT_LEFT, 105);
    cacheList->InsertColumn(kColumnMac, L"Ethernet 주소", LVCFMT_LEFT, 125);
    cacheList->InsertColumn(kColumnState, L"상태", LVCFMT_LEFT, 80);
    cacheList->InsertColumn(kColumnTime, L"남은 시간", LVCFMT_LEFT, 70);
    SetTimer(kCacheTimer, kCacheTimerMs, nullptr);

    adapters_ = ni_.Enumerate();
    auto* adapterCombo = static_cast<CComboBox*>(GetDlgItem(IDC_COMBO_ADAPTER));
    for (const NetworkAdapter& adapter : adapters_) {
        const std::wstring label =
            adapter.description + L" [" + Wire::MacText(adapter.mac) + L"]";
        adapterCombo->AddString(label.c_str());
    }

    if (!adapters_.empty()) {
        adapterCombo->SetCurSel(0);
        OnAdapter();
    } else {
        Queue(L"어댑터가 없습니다. Npcap 설치와 권한을 확인하세요.");
    }

    Ready(false);
    Queue(L"실제 Ethernet NIC와 내 IP 주소를 설정하고 주소 설정을 누르세요.");
    return TRUE;
}

void CMACFileTransferDlg::Ready(bool state) {
    ready_ = state;

    GetDlgItem(IDC_BUTTON_SEND)->EnableWindow(state);
    GetDlgItem(IDC_EDIT_MSG)->EnableWindow(state);
    GetDlgItem(IDC_BUTTON_FILE_SEND)->EnableWindow(state);
    GetDlgItem(IDC_BUTTON_ARP_SEND)->EnableWindow(state);

    GetDlgItem(IDC_COMBO_ADAPTER)->EnableWindow(!state);
    GetDlgItem(IDC_EDIT_SRC)->EnableWindow(!state);
    GetDlgItem(IDC_IP_SRC)->EnableWindow(!state);

    SetDlgItemText(IDC_BUTTON_ADDR, state ? L"연결 해제" : L"주소 설정");
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

    if (!adapter.hasMac) {
        Queue(L"MAC 자동 조회 실패: 선택한 NIC의 실제 MAC을 직접 입력하세요.");
    }

    auto* ipControl = static_cast<CIPAddressCtrl*>(GetDlgItem(IDC_IP_SRC));
    if (adapter.hasIp) {
        ipControl->SetAddress(adapter.ip[0], adapter.ip[1], adapter.ip[2], adapter.ip[3]);
    } else {
        ipControl->ClearAddress();
    }
}

void CMACFileTransferDlg::OnAddress() {
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

    // 목적지 MAC은 ARP로 알아낸 뒤 입력해도 되므로 주소 설정 시에는 선택 사항이다.
    ethernet_.SetSourceAddress(source);
    ethernet_.SetDestinAddress({});
    ip_.SetSourceAddress(myIp);
    arp_.SetSourceAddress(source, myIp);

    if (ni_.Open(adapter)) {
        Ready(true);
        Queue(L"주소 설정 완료: " + Wire::IpText(myIp) + L" / " + Wire::MacText(source));
    } else {
        AfxMessageBox(
            L"장치를 열지 못했습니다. 아래 상태와 Npcap 설정을 확인하세요.");
    }
}

void CMACFileTransferDlg::OnSend() {
    if (!ready_) {
        return;
    }

    CString text;
    GetDlgItemText(IDC_EDIT_MSG, text);
    if (text.IsEmpty() || !ApplyDestination()) {
        return;
    }

    if (!chat_.StartSend(Wire::Utf8(text.GetString()))) {
        AfxMessageBox(
            L"이전 채팅을 송신 중이거나 송신을 시작하지 못했습니다.");
        return;
    }

    Queue(
        L"[" +
        Wire::MacText(ethernet_.GetSourceAddress()) +
        L":" +
        Wire::MacText(ethernet_.GetDestinAddress()) +
        L"] " +
        text.GetString());
    SetDlgItemText(IDC_EDIT_MSG, L"");
}

void CMACFileTransferDlg::OnBrowse() {
    CFileDialog dialog(
        TRUE,
        nullptr,
        nullptr,
        OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
        L"모든 파일 (*.*)|*.*||",
        this);

    if (dialog.DoModal() == IDOK) {
        filePath_ = dialog.GetPathName().GetString();
        SetDlgItemText(IDC_EDIT_FILE, filePath_.c_str());
    }
}

void CMACFileTransferDlg::OnFileSend() {
    if (!ready_ || !ApplyDestination()) {
        return;
    }

    if (!file_.StartSend(filePath_)) {
        AfxMessageBox(
            L"파일을 선택하세요. "
            L"전송 중에는 다른 파일 전송을 시작할 수 없습니다.");
    }
}

bool CMACFileTransferDlg::ApplyDestination() {
    CString destinationText;
    GetDlgItemText(IDC_EDIT_DST, destinationText);

    MacAddress destination{};
    const bool validDestination =
        Wire::ParseMac(destinationText.GetString(), destination) &&
        (destination[0] & 1) == 0 &&
        destination != ethernet_.GetSourceAddress();
    if (!validDestination) {
        AfxMessageBox(
            L"상대 PC의 유니캐스트 MAC 주소를 입력하세요. "
            L"ARP 캐시 항목을 더블클릭하면 자동으로 입력됩니다.");
        return false;
    }

    // 진행 중인 송신의 나머지 조각이 다른 호스트로 가지 않도록 한다.
    const bool busy = chat_.Busy() || file_.Busy();
    if (busy && destination != ethernet_.GetDestinAddress()) {
        AfxMessageBox(L"송신 중에는 목적지 MAC을 바꿀 수 없습니다.");
        return false;
    }

    ethernet_.SetDestinAddress(destination);
    return true;
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
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    IpAddress target{};
    if (selected < 0 ||
        !Wire::ParseIp(cacheList->GetItemText(selected, kColumnIp).GetString(), target)) {
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

void CMACFileTransferDlg::OnArpDoubleClick(NMHDR*, LRESULT* result) {
    *result = 0;
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    MacAddress mac{};
    if (selected >= 0 &&
        Wire::ParseMac(cacheList->GetItemText(selected, kColumnMac).GetString(), mac)) {
        SetDlgItemText(IDC_EDIT_DST, Wire::MacText(mac).c_str());
    }
}

void CMACFileTransferDlg::OnTimer(UINT_PTR id) {
    if (id != kCacheTimer) {
        CDialogEx::OnTimer(id);
        return;
    }

    // 수신 스레드가 멈춘 상태에서도 만료를 처리하고 남은 시간을 갱신한다.
    arp_.Expire(GetTickCount64());
    RefreshCache();
}

void CMACFileTransferDlg::RefreshCache() {
    auto* cacheList = static_cast<CListCtrl*>(GetDlgItem(IDC_LIST_ARP));
    const int selected = cacheList->GetNextItem(-1, LVNI_SELECTED);
    const CString selectedIp =
        selected >= 0 ? cacheList->GetItemText(selected, kColumnIp) : CString();

    const std::vector<CARPLayer::CacheEntry> entries = arp_.Snapshot();
    const ULONGLONG now = GetTickCount64();

    cacheList->SetRedraw(FALSE);
    cacheList->DeleteAllItems();
    for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
        const CARPLayer::CacheEntry& entry = entries[index];
        const bool complete = entry.state == CARPLayer::EntryState::Complete;
        const ULONGLONG remainingSeconds =
            entry.expires > now ? (entry.expires - now + 999) / 1000 : 0;

        wchar_t remaining[16];
        swprintf_s(
            remaining,
            L"%02llu:%02llu",
            remainingSeconds / 60,
            remainingSeconds % 60);

        const std::wstring ip = Wire::IpText(entry.ip);
        cacheList->InsertItem(index, ip.c_str());
        cacheList->SetItemText(
            index,
            kColumnMac,
            complete ? Wire::MacText(entry.mac).c_str() : L"???????");
        cacheList->SetItemText(index, kColumnState, complete ? L"Complete" : L"Incomplete");
        cacheList->SetItemText(index, kColumnTime, remaining);

        if (selectedIp == ip.c_str()) {
            cacheList->SetItemState(index, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    cacheList->SetRedraw(TRUE);
    cacheList->Invalidate();
}

bool CMACFileTransferDlg::ReceiveFrame(
    const unsigned char* data,
    size_t length,
    const FrameContext& context) {
    const std::wstring text = Wire::Wide(std::string(
        reinterpret_cast<const char*>(data),
        length));
    if (text.empty()) {
        return false;
    }

    Queue(
        L"[" +
        Wire::MacText(context.source) +
        L":" +
        Wire::MacText(context.destination) +
        L"] " +
        text);
    return true;
}

void CMACFileTransferDlg::Queue(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (closing_) {
        return;
    }

    // UI가 잠시 지연되어도 진행 이벤트가 무한히 쌓이지 않도록 제한한다.
    if (events_.size() >= kMaximumQueuedEvents) {
        events_.pop_front();
    }

    const bool shouldWakeUi = events_.empty();
    events_.push_back(message);
    if (shouldWakeUi && GetSafeHwnd() != nullptr) {
        PostMessage(WM_NETWORK_EVENT);
    }
}

void CMACFileTransferDlg::QueueProgress(const CFileAppLayer::Progress& progress) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (closing_) return;
    const size_t index = progress.sending ? 0 : 1;
    const bool shouldWakeUi = !progressPending_[index];
    progress_[index] = progress;
    progressPending_[index] = true;
    if (shouldWakeUi && GetSafeHwnd() != nullptr) PostMessage(WM_NETWORK_EVENT);
}

void CMACFileTransferDlg::QueueCacheRefresh() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (closing_) return;
    const bool shouldWakeUi = !cachePending_;
    cachePending_ = true;
    if (shouldWakeUi && GetSafeHwnd() != nullptr) PostMessage(WM_NETWORK_EVENT);
}

void CMACFileTransferDlg::ShowProgress(const CFileAppLayer::Progress& progress) {
    auto* bar = GetDlgItem(progress.sending ? IDC_PROGRESS_SEND : IDC_PROGRESS_RECEIVE);
    bar->SendMessage(PBM_SETSTATE, progress.state == CFileAppLayer::ProgressState::Failed ? PBST_ERROR : PBST_NORMAL);
    bar->SendMessage(PBM_SETPOS, progress.percent);
    std::wstring label = progress.sending ? L"송신 " : L"수신 ";
    label += std::to_wstring(progress.percent) + L"%";
    if (progress.state == CFileAppLayer::ProgressState::Complete) label += L" 완료";
    if (progress.state == CFileAppLayer::ProgressState::Failed) label += L" 중단/실패";
    SetDlgItemText(progress.sending ? IDC_STATIC_SEND_PROGRESS : IDC_STATIC_RECEIVE_PROGRESS, label.c_str());
}

LRESULT CMACFileTransferDlg::OnEvents(WPARAM, LPARAM) {
    std::deque<std::wstring> batch;
    std::array<CFileAppLayer::Progress, 2> progress;
    std::array<bool, 2> progressPending;
    bool cachePending = false;
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        batch.swap(events_);
        progress = progress_;
        progressPending = progressPending_;
        progressPending_.fill(false);
        cachePending = cachePending_;
        cachePending_ = false;
    }
    if (cachePending) {
        RefreshCache();
    }
    for (size_t index = 0; index < progress.size(); ++index) {
        if (progressPending[index]) ShowProgress(progress[index]);
    }

    auto* chatList = static_cast<CListBox*>(GetDlgItem(IDC_LIST_CHAT));
    for (const std::wstring& message : batch) {
        // ListBox 한 항목의 길이 제한을 피하면서 긴 채팅 내용은 모두 보존한다.
        for (size_t offset = 0;
             offset < message.size();
             offset += kListItemCharacters) {
            if (chatList->GetCount() >= static_cast<int>(kMaximumListItems)) {
                chatList->DeleteString(0);
            }
            chatList->AddString(
                message.substr(offset, kListItemCharacters).c_str());
        }

        SetDlgItemText(
            IDC_STATIC_STATUS,
            message.substr(0, kStatusCharacters).c_str());
    }

    chatList->SetTopIndex(std::max(0, chatList->GetCount() - 1));
    return 0;
}

void CMACFileTransferDlg::Disconnect() {
    chat_.Stop();
    file_.Stop();
    ni_.Close();
    chat_.Reset();
    file_.Reset();
    Ready(false);
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
        const int focusedControlId = ::GetDlgCtrlID(::GetFocus());

        // IP 주소 컨트롤은 내부 편집 상자가 포커스를 가지므로 부모 ID를 확인한다.
        if (::GetDlgCtrlID(::GetParent(::GetFocus())) == IDC_IP_TARGET) {
            OnArpSend();
            return TRUE;
        }

        if (focusedControlId == IDC_EDIT_MSG &&
            !(GetKeyState(VK_SHIFT) & 0x8000)) {
            OnSend();
            return TRUE;
        }

        if (focusedControlId != IDC_EDIT_MSG) {
            return TRUE;
        }
    }

    return CDialogEx::PreTranslateMessage(message);
}
