#include "pch.h"
#include "ChatAppLayer.h"
#include "FileAppLayer.h"
#include "LayerManager.h"
#include <iostream>
#include <fstream>
static int checks=0;
void Require(bool condition,const char* text) { if(!condition) throw std::runtime_error(text); ++checks; }
class Capture : public CBaseLayer {
public:
    Capture():CBaseLayer("Capture") {}
    std::vector<std::vector<unsigned char>> packets;
    BOOL Send(unsigned char* p,int n) override { packets.emplace_back(p,p+n); return TRUE; }
};
class Display : public CBaseLayer {
public:
    Display():CBaseLayer("Display") {}
    std::vector<unsigned char> text;
    bool ReceiveFrame(const unsigned char* p,size_t n,const FrameContext&) override {text.assign(p,p+n); return true;}
};
int main() {
    try {
        MacAddress a{0x02,0,0,0,0,1},b{0x02,0,0,0,0,2};
        Capture capture; CEthernetLayer tx("tx"),rx("rx");
        CChatAppLayer sender("sender"),receiver("receiver"); Display display;
        CFileAppLayer fileSender("fileSender"),fileReceiver("fileReceiver");
        tx.SetSourceAddress(a); tx.SetDestinAddress(b); rx.SetSourceAddress(b);
        tx.SetUnderLayer(&capture); tx.SetUpperUnderLayer(&sender); tx.SetUpperUnderLayer(&fileSender);
        rx.SetUpperUnderLayer(&receiver); rx.SetUpperUnderLayer(&fileReceiver); receiver.SetUpperLayer(&display);
        for(size_t n : {size_t(1),size_t(1496),size_t(1497),size_t(2992),size_t(65535),size_t(65536),size_t(70000)}) {
            capture.packets.clear(); display.text.clear(); std::string text(n,'x');
            Require(sender.StartSend(text),"chat start"); while(sender.Busy()) Sleep(1); sender.Stop();
            Require(capture.packets.size()==1+(n+1495)/1496,"fragment count");
            for(const auto& p:capture.packets) {
                Require(p.size()>=60&&p.size()<=1514,"Ethernet size");
                Require(p[12]==0x20&&p[13]==0x80,"EtherType endian");
                Require(rx.ReceiveFrame(p.data(),p.size(),{}),"chat receive");
            }
            Require(std::string(display.text.begin(),display.text.end())==text,"chat roundtrip");
        }
        auto bad=capture.packets.front(); bad[0]=4;
        Require(!rx.ReceiveFrame(bad.data(),bad.size(),{}),"wrong destination");
        bad=capture.packets.front(); std::fill(bad.begin(),bad.begin()+6,0xff);
        Require(!rx.ReceiveFrame(bad.data(),bad.size(),{}),"all-FF destination rejected");
        bad=capture.packets.front(); std::copy(b.begin(),b.end(),bad.begin()+6);
        Require(!rx.ReceiveFrame(bad.data(),bad.size(),{}),"self source");
        Require(!rx.ReceiveFrame(bad.data(),13,{}),"truncated Ethernet");
        Require(!receiver.ReceiveFrame(bad.data(),3,{}),"truncated chat");
        // Drop one full middle fragment; final cannot complete the message.
        receiver.Reset(); display.text.clear();
        for(size_t i=0;i<capture.packets.size();++i) if(i!=1) rx.ReceiveFrame(capture.packets[i].data(),capture.packets[i].size(),{});
        Require(display.text.empty(),"lost chat fragment must not complete");
        MacAddress parsed{}; Require(Wire::ParseMac(L"02:00:00:00:00:01",parsed)&&parsed==a,"MAC parse");
        Require(!Wire::ParseMac(L"02:00:00:00:00:GG",parsed),"invalid MAC");
        Require(Wire::Wide(Wire::Utf8(L"한글 채팅 테스트"))==L"한글 채팅 테스트","UTF8 roundtrip");
        std::wstring out=L"protocol-test-files-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        Require(CreateDirectoryW(out.c_str(),nullptr)!=FALSE,"test output directory"); fileReceiver.SetDirectory(out);
        std::vector<CFileAppLayer::Progress> sendProgress, receiveProgress;
        fileSender.onProgress=[&](const CFileAppLayer::Progress& p){sendProgress.push_back(p);};
        fileReceiver.onProgress=[&](const CFileAppLayer::Progress& p){receiveProgress.push_back(p);};
        for(size_t n : {size_t(0),size_t(1),size_t(1488),size_t(1489),size_t(8192)}) {
            sendProgress.clear(); receiveProgress.clear();
            std::wstring path=out+L"\\input_"+std::to_wstring(n)+L".bin";
            std::vector<unsigned char> data(n); for(size_t i=0;i<n;++i) data[i]=(unsigned char)(i%251);
            { std::ofstream f(path,std::ios::binary); f.write((const char*)data.data(),data.size()); }
            capture.packets.clear(); std::wstring result;
            std::wstring lastStatus;
            fileReceiver.notify=[&](const std::wstring& s){lastStatus=s; if(s.find(L"파일 수신 완료: ")==0) result=s.substr(10);};
            Require(fileSender.StartSend(path),"file start"); while(fileSender.Busy()) Sleep(1); fileSender.Stop();
            Require(capture.packets.size()==2+(n+1487)/1488,"file fragments");
            for(const auto& p:capture.packets) {
                if(!rx.ReceiveFrame(p.data(),p.size(),{})) {
                    std::cerr<<"File receive failed for "<<n<<" bytes, frame="<<p.size()
                             <<", etherType="<<Wire::Read16(p.data()+12)
                             <<", fileType="<<Wire::Read16(p.data()+18)
                             <<", messageType="<<(int)p[20]<<", unused="<<(int)p[21]
                             <<", seq="<<Wire::Read32(p.data()+22)
                             <<", status="<<Wire::Utf8(lastStatus)<<"\n";
                    Require(false,"file receive");
                }
                Require(true,"file receive");
            }
            Require(!result.empty(),"file completion event");
            std::ifstream f(result,std::ios::binary); std::vector<unsigned char> actual((std::istreambuf_iterator<char>(f)),{});
            Require(actual==data,"binary file roundtrip");
            Require(!sendProgress.empty() && sendProgress.front().sending && sendProgress.front().percent==0,"send progress starts at zero");
            Require(sendProgress.back().percent==100 && sendProgress.back().state==CFileAppLayer::ProgressState::Complete,"send progress completes including empty files");
            Require(!receiveProgress.empty() && !receiveProgress.front().sending && receiveProgress.front().percent==0,"receive progress starts at zero");
            Require(receiveProgress.back().percent==100 && receiveProgress.back().state==CFileAppLayer::ProgressState::Complete,"receive progress completes including empty files");
            for(const auto* progress : {&sendProgress,&receiveProgress}) {
                for(size_t i=1;i<progress->size();++i) {
                    Require((*progress)[i].percent>=(*progress)[i-1].percent && (*progress)[i].percent<=100,"progress monotonic and bounded");
                }
            }
        }
        // The first file frame must create a .part file at its complete logical size.
        std::wstring prealloc=out+L"\\prealloc_"+std::to_wstring(GetTickCount64());
        Require(CreateDirectoryW(prealloc.c_str(),nullptr)!=FALSE,"preallocation test directory");
        fileReceiver.Reset(); fileReceiver.SetDirectory(prealloc);
        auto& first=capture.packets.front();
        Require(rx.ReceiveFrame(first.data(),first.size(),{}),"file metadata for preallocation");
        WIN32_FIND_DATAW partData{};
        HANDLE partFind=FindFirstFileW((prealloc+L"\\*.part").c_str(),&partData);
        Require(partFind!=INVALID_HANDLE_VALUE,"preallocated part exists");
        ULARGE_INTEGER partSize{}; partSize.LowPart=partData.nFileSizeLow; partSize.HighPart=partData.nFileSizeHigh;
        FindClose(partFind);
        Require(partSize.QuadPart==8192,"part file preallocated to announced size");
        fileReceiver.Reset();
        Require(RemoveDirectoryW(prealloc.c_str())!=FALSE,"preallocation test cleanup");
        fileReceiver.SetDirectory(out);
        // Sequence mismatch must reject incomplete files.
        Require(rx.ReceiveFrame(first.data(),first.size(),{}),"new file metadata");
        auto wrong=capture.packets[1]; Wire::Write32(wrong.data()+14+8,99);
        Require(!rx.ReceiveFrame(wrong.data(),wrong.size(),{}),"file sequence rejection");
        Require(receiveProgress.back().state==CFileAppLayer::ProgressState::Failed,"failed receive progress does not show completion");
        fileReceiver.Reset();
        sendProgress.clear();
        Require(fileSender.StartSend(out+L"\\missing.bin"),"missing file worker starts");
        while(fileSender.Busy()) Sleep(1); fileSender.Stop();
        Require(sendProgress.back().percent==0 && sendProgress.back().state==CFileAppLayer::ProgressState::Failed,"failed send progress remains at zero");
        fileSender.onProgress=nullptr; fileReceiver.onProgress=nullptr;
        std::cout<<"PASS: "<<checks<<" assertions; no NIC opened, no network packets sent.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n"; return 1;}
}
