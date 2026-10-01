#pragma once
#include "EthernetLayer.h"

class CFileAppLayer : public CBaseLayer {
public:
    enum class ProgressState { Active, Complete, Failed };
    struct Progress {
        bool sending = true;
        int percent = 0;
        ProgressState state = ProgressState::Active;
    };
    std::function<void(const Progress&)> onProgress;
    explicit CFileAppLayer(const char* name)
        : CBaseLayer(name) {
    }

    ~CFileAppLayer() override {
        Stop();
        onProgress = nullptr;
        Reset();
    }

    bool StartSend(const std::wstring& path);
    void Stop();
    void Reset();

    bool Busy() const { return busy_; }

    void SetDirectory(const std::wstring& directory) {
        directory_ = directory;
    }

    bool ReceiveFrame(const unsigned char*, size_t, const FrameContext&) override;
    void OnIdle() override;

private:
    struct FileTransferContext {
        CFileAppLayer* layer = nullptr;
        std::wstring path;
    };

    struct Incoming {
        HANDLE file = INVALID_HANDLE_VALUE;
        uint64_t total = 0;
        uint64_t received = 0;
        uint32_t sequence = 1;
        unsigned char flag = 0;
        ULONGLONG updated = 0;
        std::wstring partial;
        std::wstring finalPath;

        ~Incoming() {
            if (file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
            }
            if (!partial.empty()) {
                DeleteFileW(partial.c_str());
            }
        }
    };

    using TransferKey = std::pair<MacAddress, MacAddress>;

    // 과제 명세의 함수 원형을 유지한다.
    // AfxBeginThread가 요구하는 LPVOID 원형은 Entry 함수가 변환한다.
    static UINT AFX_CDECL FileTransferThread(LPDWORD lpdwParam);
    static UINT AFX_CDECL FileTransferThreadEntry(LPVOID parameter);

    void JoinWorker();
    bool SendFileFrames(const std::wstring& path);
    void ReportProgress(bool sending, int percent, ProgressState state = ProgressState::Active) {
        if (onProgress) onProgress({sending, percent, state});
    }

    std::map<TransferKey, std::unique_ptr<Incoming>> incoming_;
    std::wstring directory_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
    CWinThread* worker_ = nullptr;
    int sentPercent_ = 0;
};
