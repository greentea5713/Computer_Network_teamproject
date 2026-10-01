#include "pch.h"
#include "framework.h"
#include "MACFileTransfer.h"
#include "MACFileTransferDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CMACFileTransferApp, CWinApp)
    ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()

CMACFileTransferApp::CMACFileTransferApp() {
    m_dwRestartManagerSupportFlags = AFX_RESTART_MANAGER_SUPPORT_RESTART;
}

CMACFileTransferApp theApp;

BOOL CMACFileTransferApp::InitInstance() {
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&commonControls);

    CWinApp::InitInstance();
    AfxEnableControlContainer();

    auto* shellManager = new CShellManager;
    CMFCVisualManager::SetDefaultManager(
        RUNTIME_CLASS(CMFCVisualManagerWindows));
    SetRegistryKey(_T("로컬 애플리케이션 마법사에서 생성된 애플리케이션"));

    CMACFileTransferDlg dialog;
    m_pMainWnd = &dialog;
    const INT_PTR response = dialog.DoModal();

    if (response == -1) {
        TRACE(
            traceAppMsg,
            0,
            "대화 상자를 만들지 못해 애플리케이션을 종료합니다.\n");
    }

    delete shellManager;

#if !defined(_AFXDLL) && !defined(_AFX_NO_MFC_CONTROLS_IN_DIALOGS)
    ControlBarCleanUp();
#endif

    // 대화 상자 기반 프로그램이므로 창을 닫으면 메시지 루프를 시작하지 않고 종료한다.
    return FALSE;
}
