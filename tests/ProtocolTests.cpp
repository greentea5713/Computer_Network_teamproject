#include "pch.h"
#include "ChatAppLayer.h"
#include "FileAppLayer.h"
#include "LayerManager.h"
#include "IPLayer.h"
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

        // ARP: Host A(a, ipA)가 Host B(b, ipB)의 MAC을 요청하고 응답을 받는다.
        IpAddress ipA{168,188,129,63}, ipB{168,188,129,2}, ipC{168,188,129,58};
        IpAddress parsedIp{};
        Require(Wire::ParseIp(L"168.188.129.2",parsedIp)&&parsedIp==ipB,"IP parse");
        Require(!Wire::ParseIp(L"168.188.129.256",parsedIp)&&!Wire::ParseIp(L"1.2.3",parsedIp)&&!Wire::ParseIp(L"1.2.3.4.",parsedIp),"invalid IP");
        Require(Wire::IpText(ipB)==L"168.188.129.2","IP text");
        Capture captureA, captureB; CEthernetLayer ethA("ethA"), ethB("ethB");
        CARPLayer arpA("arpA"), arpB("arpB"); CIPLayer ipLayerA("ipA");
        ethA.SetSourceAddress(a); ethB.SetSourceAddress(b);
        ethA.SetUnderLayer(&captureA); ethB.SetUnderLayer(&captureB);
        CChatAppLayer chatA("chatA"), chatB("chatB"); CFileAppLayer fileA("fileA"), fileB("fileB");
        ethA.SetUpperUnderLayer(&chatA); ethA.SetUpperUnderLayer(&fileA); ethA.SetUpperUnderLayer(&arpA);
        ethB.SetUpperUnderLayer(&chatB); ethB.SetUpperUnderLayer(&fileB); ethB.SetUpperUnderLayer(&arpB);
        ethA.SetUpperUnderLayer(&ipLayerA); ipLayerA.SetArpLayer(&arpA);
        arpA.SetSourceAddress(a,ipA); arpB.SetSourceAddress(b,ipB); ipLayerA.SetSourceAddress(ipA);
        int changesA=0; arpA.onCacheChanged=[&]{++changesA;};

        Require(ipLayerA.Request(ipB),"ARP request sent");
        Require(captureA.packets.size()==1,"one ARP request frame");
        const auto request=captureA.packets.back();
        Require(request.size()==60,"ARP request padded to minimum Ethernet frame");
        Require(std::equal(request.begin(),request.begin()+6,Wire::BroadcastMac.begin()),"ARP request broadcast");
        Require(std::equal(request.begin()+6,request.begin()+12,a.begin()),"ARP request Ethernet source");
        Require(Wire::Read16(request.data()+12)==0x0806,"ARP EtherType");
        const unsigned char* arp=request.data()+14;
        Require(Wire::Read16(arp)==1&&Wire::Read16(arp+2)==0x0800&&arp[4]==6&&arp[5]==4,"ARP hardware/protocol fields");
        Require(Wire::Read16(arp+6)==1,"ARP request opcode");
        Require(std::equal(arp+8,arp+14,a.begin())&&std::equal(arp+14,arp+18,ipA.begin()),"ARP request sender");
        Require(std::all_of(arp+18,arp+24,[](unsigned char x){return x==0;})&&std::equal(arp+24,arp+28,ipB.begin()),"ARP request target");
        auto cacheA=arpA.Snapshot();
        Require(cacheA.size()==1&&cacheA[0].ip==ipB&&cacheA[0].state==CARPLayer::EntryState::Incomplete,"incomplete entry after request");
        Require(changesA>0,"cache change notification");
        MacAddress resolved{};
        Require(!arpA.Lookup(ipB,resolved),"incomplete entry is not resolved");

        Require(ethB.ReceiveFrame(request.data(),request.size(),{}),"ARP broadcast accepted");
        auto cacheB=arpB.Snapshot();
        Require(cacheB.size()==1&&cacheB[0].ip==ipA&&cacheB[0].mac==a&&cacheB[0].state==CARPLayer::EntryState::Complete,"requested host learns sender");
        Require(captureB.packets.size()==1,"one ARP reply frame");
        const auto reply=captureB.packets.back();
        Require(std::equal(reply.begin(),reply.begin()+6,a.begin())&&std::equal(reply.begin()+6,reply.begin()+12,b.begin()),"ARP reply unicast");
        const unsigned char* rep=reply.data()+14;
        Require(Wire::Read16(rep+6)==2,"ARP reply opcode");
        Require(std::equal(rep+8,rep+14,b.begin())&&std::equal(rep+14,rep+18,ipB.begin()),"ARP reply sender swapped");
        Require(std::equal(rep+18,rep+24,a.begin())&&std::equal(rep+24,rep+28,ipA.begin()),"ARP reply target swapped");

        Require(ethA.ReceiveFrame(reply.data(),reply.size(),{}),"ARP reply received");
        Require(arpA.Lookup(ipB,resolved)&&resolved==b,"ARP reply completes entry");
        Require(ipLayerA.Resolve(ipB,resolved)&&resolved==b&&captureA.packets.size()==1,"IP resolves from cache without new request");
        cacheA=arpA.Snapshot();
        Require(cacheA.size()==1&&cacheA[0].state==CARPLayer::EntryState::Complete,"complete entry");

        // B는 자기 IP가 아닌 요청에는 응답하지 않고, 모르는 송신자를 새로 추가하지 않는다.
        MacAddress c{0x02,0,0,0,0,3}; auto other=request;
        std::copy(c.begin(),c.end(),other.begin()+6); std::copy(c.begin(),c.end(),other.begin()+14+8);
        std::copy(ipC.begin(),ipC.end(),other.begin()+14+14); std::copy(ipA.begin(),ipA.end(),other.begin()+14+24);
        Require(ethB.ReceiveFrame(other.data(),other.size(),{}),"ARP request for other host parsed");
        Require(captureB.packets.size()==1&&arpB.Snapshot().size()==1,"no reply or new entry for other host");
        // 이미 캐시에 있는 송신자의 주소가 바뀌면 갱신한다.
        auto moved=request; MacAddress a2{0x02,0,0,0,0,9};
        std::copy(a2.begin(),a2.end(),moved.begin()+6); std::copy(a2.begin(),a2.end(),moved.begin()+14+8);
        std::copy(ipC.begin(),ipC.end(),moved.begin()+14+24);
        Require(ethB.ReceiveFrame(moved.data(),moved.size(),{}),"ARP update parsed");
        Require(arpB.Lookup(ipA,resolved)&&resolved==a2,"existing entry updated");

        auto malformed=request; Wire::Write16(malformed.data()+14+2,0x86DD);
        Require(!ethB.ReceiveFrame(malformed.data(),malformed.size(),{}),"non-IPv4 ARP rejected");
        Require(!arpB.ReceiveFrame(request.data()+14,27,{}),"truncated ARP rejected");
        auto chatBroadcast=request; Wire::Write16(chatBroadcast.data()+12,Wire::ChatType);
        Require(!ethB.ReceiveFrame(chatBroadcast.data(),chatBroadcast.size(),{}),"non-ARP broadcast still rejected");

        // 캐시 타임아웃: Complete 20분, Incomplete 3분
        Require(ipLayerA.Request(ipC),"second ARP request");
        const ULONGLONG now=GetTickCount64();
        arpA.Expire(now+60*1000);
        Require(arpA.Snapshot().size()==2,"entries kept before timeout");
        arpA.Expire(now+CARPLayer::kIncompleteTimeoutMs+1000);
        cacheA=arpA.Snapshot();
        Require(cacheA.size()==1&&cacheA[0].ip==ipB,"incomplete entry expires after 3 minutes");
        arpA.Expire(now+CARPLayer::kCompleteTimeoutMs+1000);
        Require(arpA.Snapshot().empty(),"complete entry expires after 20 minutes");

        Require(!ipLayerA.Resolve(ipB,resolved)&&captureA.packets.size()==3,"cache miss sends ARP request");
        Require(arpA.Remove(ipB)&&!arpA.Remove(ipB),"remove entry");
        ipLayerA.Request(ipB); ipLayerA.Request(ipC); arpA.Clear();
        Require(arpA.Snapshot().empty(),"clear cache");
        Require(!ipLayerA.Request(ipA),"no request for own IP");
        std::cout<<"PASS: "<<checks<<" assertions; no NIC opened, no network packets sent.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n"; return 1;}
}
