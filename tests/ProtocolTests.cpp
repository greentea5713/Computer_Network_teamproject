#include "pch.h"
#include "LayerManager.h"
#include "IPLayer.h"
#include "RouterCore.h"
#include "NatCore.h"
#include <iostream>
static int checks=0;
void Require(bool condition,const char* text) { if(!condition) throw std::runtime_error(text); ++checks; }
class Capture : public CBaseLayer {
public:
    Capture():CBaseLayer("Capture") {}
    std::vector<std::vector<unsigned char>> packets;
    BOOL Send(unsigned char* p,int n) override { packets.emplace_back(p,p+n); return TRUE; }
};

// Independent reference checksum over the TCP/UDP pseudo-header and payload.
uint16_t ReferenceTransport(const std::vector<unsigned char>& packet) {
    const size_t header = (packet[0] & 15) * 4;
    std::vector<unsigned char> bytes;
    if (packet[9] != NatCore::Icmp) {
        bytes.assign(packet.begin() + 12, packet.begin() + 20);
        bytes.insert(bytes.end(), {0, packet[9], 0, 0});
        Wire::Write16(bytes.data() + 10, static_cast<uint16_t>(packet.size() - header));
    }
    bytes.insert(bytes.end(), packet.begin() + header, packet.end());
    return RouterCore::Checksum(bytes.data(), bytes.size());
}

std::vector<unsigned char> Datagram(unsigned char protocol, const IpAddress& src, const IpAddress& dst,
                                    uint16_t from, uint16_t to, unsigned char typeOrFlags) {
    constexpr size_t header = 24; // Include options and an odd transport length.
    const size_t payloadHeader = protocol == NatCore::Tcp ? 20 : 8;
    std::vector<unsigned char> packet(header + payloadHeader + 1);
    packet[0] = 0x46;
    packet[8] = 128;
    packet[9] = protocol;
    Wire::Write16(packet.data() + 2, static_cast<uint16_t>(packet.size()));
    std::copy(src.begin(), src.end(), packet.begin() + 12);
    std::copy(dst.begin(), dst.end(), packet.begin() + 16);
    auto* payload = packet.data() + header;
    auto* checksum = payload + 2;
    if (protocol == NatCore::Icmp) {
        payload[0] = typeOrFlags;
        Wire::Write16(payload + 4, from);
        Wire::Write16(payload + 6, 123);
    } else {
        Wire::Write16(payload, from);
        Wire::Write16(payload + 2, to);
        if (protocol == NatCore::Tcp) {
            payload[12] = 0x50;
            payload[13] = typeOrFlags;
            checksum = payload + 16;
        } else {
            Wire::Write16(payload + 4, static_cast<uint16_t>(payloadHeader + 1));
            checksum = payload + 6;
        }
    }
    packet.back() = 0xa7;
    const auto value = ReferenceTransport(packet);
    Wire::Write16(checksum, protocol == NatCore::Udp && !value ? 0xffff : value);
    Wire::Write16(packet.data() + 10, RouterCore::Checksum(packet.data(), header));
    return packet;
}

void TestNat() {
    using namespace NatCore;
    const IpAddress lan{192,168,42,1}, wan{10,42,0,2}, host{192,168,42,10},
        second{192,168,42,11}, remote{172,16,42,10}, other{172,16,42,11}, gateway{10,42,0,1};
    const std::vector<RouteEntry> routes{
        {{192,168,42,0},{255,255,255,0},{},Inside,false},
        {{172,16,42,0},{255,255,255,0},gateway,Outside,true}
    };
    std::vector<unsigned char> result;
    IpAddress destination{};
    RouteEntry route;
    uint64_t now = 0;
    Table nat;
    const auto send = [&](int ingress, const auto& packet) {
        if (!RouterCore::Prepare(packet.data(), packet.size(), result) ||
            !nat.Forward(result, ingress, lan, wan, routes, now, route, destination)) return false;
        Require(result[8] == 127 && result.back() == 0xa7 &&
                RouterCore::Checksum(result.data(), 24) == 0, "NAT preserves payload and IPv4 options, updates TTL/checksum");
        if (result[9] != Udp || Wire::Read16(result.data() + 30)) {
            Require(ReferenceTransport(result) == 0, "NAT checksum verifies with independent pseudo-header");
        }
        return true;
    };
    const auto value = [&](int offset) { return Wire::Read16(result.data() + 24 + offset); };
    Require(send(Inside, Datagram(Icmp, host, remote, 7, 0, 8)) &&
            Address(result.data() + 12) == wan && route.viaGateway && route.gateway == gateway,
            "outbound ICMP with Cisco gateway");
    const auto id = value(4);
    Require(id != 7 && value(6) == 123, "replace Identifier, preserve echo sequence");
    Require(send(Inside, Datagram(Icmp, second, remote, 7, 0, 8)) && value(4) != id,
            "two hosts sharing ICMP Identifier obtain distinct external IDs");
    Require(send(Inside, Datagram(Icmp, host, remote, 7, 0, 8)) && value(4) == id,
            "repeat echo reuses mapping");
    now = ShortIdleMs - 1;
    Require(send(Outside, Datagram(Icmp, remote, wan, id, 0, 0)) &&
            destination == host && route.port == Inside && Address(result.data() + 16) == host &&
            value(4) == 7 && value(6) == 123, "inbound echo is restored and routed after DNAT");
    now += 2;
    nat.Expire(now);
    Require(nat.Mappings().size() == 1, "reply refreshes only the matching timer");
    now += ShortIdleMs;
    Require(!send(Outside, Datagram(Icmp, remote, wan, id, 0, 0)) && nat.Mappings().empty(),
            "late reply rejected after expiration");
    Require(!send(Outside, Datagram(Icmp, remote, host, 1, 0, 8)),
            "unsolicited direct access to inside rejected");

    for (const auto protocol : {Tcp, Udp}) {
        Require(send(Inside, Datagram(protocol, host, remote, 12345, 8080, 2)) &&
                Address(result.data() + 12) == wan && value(2) == 8080, "outbound TCP/UDP translation");
        const auto external = value(0);
        Require(send(Inside, Datagram(protocol, second, remote, 12345, 8080, 2)) && value(0) != external,
                "TCP/UDP source port collision avoided");
        Require(!send(Outside, Datagram(protocol, other, wan, 8080, external, 0x12)),
                "unrelated peer cannot use dynamic TCP/UDP mapping");
        Require(send(Outside, Datagram(protocol, remote, wan, 8080, external, 0x12)) &&
                destination == host && value(2) == 12345, "TCP/UDP reply restores original endpoint");
        Require(!nat.AddRule({protocol, external, host, 9000}), "rule cannot overwrite live dynamic port");
        Require(nat.AddRule({protocol, 8090, host, 9000}) &&
                !nat.AddRule({protocol, 8090, second, 9001}), "forwarding rule and conflict");
        if (protocol == Tcp) {
            Require(!send(Outside, Datagram(Tcp, remote, wan, 47000, 8090, 0x10)),
                    "first forwarded TCP segment must be SYN");
        }
        Require(send(Outside, Datagram(protocol, remote, wan, 47000, 8090, 2)) &&
                destination == host && value(2) == 9000, "forward external port to inside service");
        Require(send(Inside, Datagram(protocol, host, remote, 9000, 47000, 0x12)) &&
                Address(result.data() + 12) == wan && value(0) == 8090,
                "reply uses advertised forwarding port without allocating another mapping");
        nat.DeleteRule(protocol, 8090);
        Require(!send(Outside, Datagram(protocol, remote, wan, 47000, 8090, 2)),
                "deleting rule revokes inbound forwarding");
    }
    auto uncheckedUdp = Datagram(Udp, host, remote, 14000, 8080, 0);
    Wire::Write16(uncheckedUdp.data() + 30, 0);
    Require(send(Inside, uncheckedUdp) && value(6) == 0, "preserve intentionally absent IPv4 UDP checksum");
    auto damaged = Datagram(Tcp, host, remote, 12345, 8080, 2);
    damaged.back() ^= 1;
    Require(!send(Inside, damaged), "reject damaged transport payload");
    auto fragment = Datagram(Udp, host, remote, 12345, 8080, 0);
    Wire::Write16(fragment.data() + 6, 0x2000);
    Wire::Write16(fragment.data() + 10, 0);
    Wire::Write16(fragment.data() + 10, RouterCore::Checksum(fragment.data(), 24));
    Require(!send(Inside, fragment), "reject fragmented datagram at the NAT boundary");
}

int main() {
    try {
        TestNat();
        {
            RouteEntry def{{0,0,0,0},{0,0,0,0},{10,0,0,1},1,true};
            RouteEntry direct{{192,168,1,0},{255,255,255,0},{},0,false};
            RouteEntry selected;
            Require(RouterCore::Select({def,direct}, {192,168,1,42}, selected) && selected.port == 0, "longest prefix route");
            Require(RouterCore::Select({def,direct}, {172,16,0,2}, selected) && selected.viaGateway && selected.gateway == def.gateway, "default gateway route");
            RouteEntry invalid = direct; invalid.mask = {255,0,255,0};
            Require(!RouterCore::ValidRoute(invalid), "noncontiguous mask rejected");
            unsigned char datagram[20]{};
            datagram[0] = 0x45; Wire::Write16(datagram + 2, 20); datagram[8] = 64;
            Wire::Write16(datagram + 10, RouterCore::Checksum(datagram, 20));
            std::vector<unsigned char> forwarded;
            Require(RouterCore::Prepare(datagram, 20, forwarded) && forwarded[8] == 63 && RouterCore::Checksum(forwarded.data(), 20) == 0, "TTL and checksum");
            datagram[8] = 1; datagram[10] = datagram[11] = 0;
            Wire::Write16(datagram + 10, RouterCore::Checksum(datagram, 20));
            Require(!RouterCore::Prepare(datagram, 20, forwarded), "expired TTL rejected");
        }
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

        // GARP: B가 A를 캐시에 가진 상태에서 A가 MAC을 바꿔 자기 IP에 대한 요청을 보낸다.
        arpB.Clear(); captureA.packets.clear(); captureB.packets.clear();
        Require(ethB.ReceiveFrame(request.data(),request.size(),{})&&arpB.Lookup(ipA,resolved)&&resolved==a,"B knows A before GARP");
        captureB.packets.clear();
        ethA.SetSourceAddress(a2); arpA.SetSourceAddress(a2,ipA);
        Require(arpA.SendGratuitous()&&captureA.packets.size()==1,"GARP sent");
        const auto garp=captureA.packets.back(); const unsigned char* g=garp.data()+14;
        Require(std::equal(garp.begin(),garp.begin()+6,Wire::BroadcastMac.begin())&&std::equal(garp.begin()+6,garp.begin()+12,a2.begin()),"GARP broadcast from new MAC");
        Require(Wire::Read16(g+6)==1&&std::equal(g+8,g+14,a2.begin())&&std::equal(g+14,g+18,ipA.begin()),"GARP sender is new MAC and own IP");
        Require(std::all_of(g+18,g+24,[](unsigned char x){return x==0;})&&std::equal(g+24,g+28,ipA.begin()),"GARP target is own IP");
        Require(ethB.ReceiveFrame(garp.data(),garp.size(),{}),"GARP received");
        Require(arpB.Lookup(ipA,resolved)&&resolved==a2&&captureB.packets.empty(),"GARP updates cache without reply");

        // GARP에 응답이 오면 IP 중복이다. (A가 B와 같은 IP로 GARP를 보낸 경우)
        MacAddress duplicateAtA{}, duplicateAtB{};
        arpA.onDuplicateIp=[&](const MacAddress& m){duplicateAtA=m;};
        arpB.onDuplicateIp=[&](const MacAddress& m){duplicateAtB=m;};
        captureA.packets.clear(); arpA.SetSourceAddress(a2,ipB);
        Require(arpA.SendGratuitous(),"duplicate GARP sent");
        Require(ethB.ReceiveFrame(captureA.packets.back().data(),captureA.packets.back().size(),{}),"duplicate GARP received");
        Require(duplicateAtB==a2,"owner detects duplicate IP");
        Require(captureB.packets.size()==1,"owner replies to GARP for its IP");
        const auto dupReply=captureB.packets.back();
        Require(ethA.ReceiveFrame(dupReply.data(),dupReply.size(),{})&&duplicateAtA==b,"GARP sender detects duplicate IP from reply");
        arpA.onDuplicateIp=nullptr; arpB.onDuplicateIp=nullptr;
        ethA.SetSourceAddress(a); arpA.SetSourceAddress(a,ipA); arpA.Clear();

        // Proxy ARP: 내 IP가 아닌 요청이면 Proxy 테이블 항목으로 대신 응답한다.
        arpB.Clear(); captureB.packets.clear();
        MacAddress proxyMac{0x02,0,0,0,0,0x0B};
        Require(arpB.AddProxy({L"eth0",ipC,proxyMac})&&arpB.ProxySnapshot().size()==1,"proxy entry added");
        Require(!arpB.AddProxy({L"eth0",ipB,proxyMac}),"own IP cannot be proxied");
        auto forC=request; std::copy(ipC.begin(),ipC.end(),forC.begin()+14+24);
        Require(ethB.ReceiveFrame(forC.data(),forC.size(),{})&&captureB.packets.size()==1,"proxy reply sent");
        const auto proxyReply=captureB.packets.back(); const unsigned char* pr=proxyReply.data()+14;
        Require(std::equal(proxyReply.begin(),proxyReply.begin()+6,a.begin())&&std::equal(proxyReply.begin()+6,proxyReply.begin()+12,b.begin()),"proxy reply unicast from own NIC");
        Require(Wire::Read16(pr+6)==2&&std::equal(pr+8,pr+14,proxyMac.begin())&&std::equal(pr+14,pr+18,ipC.begin()),"proxy reply uses entry for proxied IP");
        Require(std::equal(pr+18,pr+24,a.begin())&&std::equal(pr+24,pr+28,ipA.begin()),"proxy reply target");
        Require(arpB.Lookup(ipA,resolved)&&resolved==a,"proxy adds requester to cache");
        Require(ethA.ReceiveFrame(proxyReply.data(),proxyReply.size(),{})&&arpA.Lookup(ipC,resolved)&&resolved==proxyMac,"requester caches proxied mapping");
        auto garpC=request; std::copy(c.begin(),c.end(),garpC.begin()+6); std::copy(c.begin(),c.end(),garpC.begin()+14+8);
        std::copy(ipC.begin(),ipC.end(),garpC.begin()+14+14); std::copy(ipC.begin(),ipC.end(),garpC.begin()+14+24);
        captureB.packets.clear();
        Require(ethB.ReceiveFrame(garpC.data(),garpC.size(),{})&&captureB.packets.empty(),"GARP is not proxied");
        Require(arpB.RemoveProxy(ipC)&&arpB.ProxySnapshot().empty(),"proxy entry removed");
        Require(ethB.ReceiveFrame(forC.data(),forC.size(),{})&&captureB.packets.empty(),"no reply after proxy removal");

        std::cout<<"PASS: "<<checks<<" assertions; no NIC opened, no network packets sent.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n"; return 1;}
}
