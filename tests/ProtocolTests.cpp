#include "pch.h"
#include "LayerManager.h"
#include "IPLayer.h"
#include <iostream>
static int checks=0;
void Require(bool condition,const char* text) { if(!condition) throw std::runtime_error(text); ++checks; }
class Capture : public CBaseLayer {
public:
    Capture():CBaseLayer("Capture") {}
    std::vector<std::vector<unsigned char>> packets;
    BOOL Send(unsigned char* p,int n) override { packets.emplace_back(p,p+n); return TRUE; }
};
int main() {
    try {
        MacAddress a{0x02,0,0,0,0,1},b{0x02,0,0,0,0,2};
        MacAddress parsed{}; Require(Wire::ParseMac(L"02:00:00:00:00:01",parsed)&&parsed==a,"MAC parse");
        Require(!Wire::ParseMac(L"02:00:00:00:00:GG",parsed),"invalid MAC");
        Require(Wire::Wide(Wire::Utf8(L"한글 테스트"))==L"한글 테스트","UTF8 roundtrip");

        // LayerManager 연결 문자열로 Dialog와 같은 구조를 만든다.
        {
            CLayerManager manager; CEthernetLayer eth("Ethernet"); CARPLayer arp("ARP"); CIPLayer ip("IP");
            CBaseLayer* layers[]={&eth,&arp,&ip};
            class Named : public CBaseLayer { public: Named():CBaseLayer("NI"){} } niNamed;
            class App : public CBaseLayer { public: App():CBaseLayer("ARPDlg"){} } appNamed;
            manager.AddLayer(&niNamed); for(auto* l:layers) manager.AddLayer(l); manager.AddLayer(&appNamed);
            manager.ConnectLayers("NI ( *Ethernet ( *ARP *IP ( +ARPDlg ) ) )");
            Require(eth.GetUpperLayer(0)==&arp&&eth.GetUpperLayer(1)==&ip,"Ethernet upper order ARP, IP");
            Require(arp.GetUnderLayer()==&eth&&ip.GetUnderLayer()==&eth&&ip.GetUpperLayer(0)==&appNamed,"layer links");
        }

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
        ethA.SetUpperUnderLayer(&arpA); ethB.SetUpperUnderLayer(&arpB);
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

        // Ethernet 필터
        auto wrongDestination=reply; wrongDestination[5]=0x7;
        Require(!ethA.ReceiveFrame(wrongDestination.data(),wrongDestination.size(),{}),"wrong destination rejected");
        Require(!ethA.ReceiveFrame(request.data(),request.size(),{}),"own frame rejected");
        Require(!ethA.ReceiveFrame(request.data(),13,{}),"truncated Ethernet");
        auto ipBroadcast=request; Wire::Write16(ipBroadcast.data()+12,Wire::IpType);
        Require(!ethB.ReceiveFrame(ipBroadcast.data(),ipBroadcast.size(),{}),"non-ARP broadcast rejected");

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

        const size_t sentBefore=captureA.packets.size();
        Require(!ipLayerA.Resolve(ipB,resolved)&&captureA.packets.size()==sentBefore+1,"cache miss sends ARP request");
        Require(arpA.Remove(ipB)&&!arpA.Remove(ipB),"remove entry");
        ipLayerA.Request(ipB); ipLayerA.Request(ipC); arpA.Clear();
        Require(arpA.Snapshot().empty(),"clear cache");
        Require(!ipLayerA.Request(ipA),"no request for own IP");

        // 해시 테이블: 여러 항목 저장/검색, 같은 키는 같은 해시, 화면용 목록은 IP 순
        Require(IpAddressHash{}(ipB)==IpAddressHash{}(IpAddress{168,188,129,2}),"same IP same hash");
        for(int i=10;i>0;--i) Require(ipLayerA.Request(IpAddress{10,0,0,(unsigned char)i}),"many requests");
        for(int i=1;i<=10;++i) { auto r=request; r[6+5]=(unsigned char)(0x40+i); std::copy(r.begin()+6,r.begin()+12,r.begin()+14+8);
            Wire::Write16(r.data()+14+6,2); std::copy(a.begin(),a.end(),r.begin()); IpAddress s{10,0,0,(unsigned char)i};
            std::copy(s.begin(),s.end(),r.begin()+14+14); std::copy(ipA.begin(),ipA.end(),r.begin()+14+24); ethA.ReceiveFrame(r.data(),r.size(),{}); }
        cacheA=arpA.Snapshot(); Require(cacheA.size()==10,"ten entries stored");
        for(int i=1;i<=10;++i) Require(arpA.Lookup(IpAddress{10,0,0,(unsigned char)i},resolved)&&resolved[5]==0x40+i,"hash lookup");
        Require(std::is_sorted(cacheA.begin(),cacheA.end(),[](const CARPLayer::CacheEntry& l,const CARPLayer::CacheEntry& r){return l.ip<r.ip;}),"snapshot sorted by IP");
        arpA.Clear();

        std::cout<<"PASS: "<<checks<<" assertions; no NIC opened, no network packets sent.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n"; return 1;}
}
