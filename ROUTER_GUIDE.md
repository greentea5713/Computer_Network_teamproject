# 정적 라우터 사용 안내

이 프로그램은 Npcap과 서로 다른 유선 Ethernet 인터페이스 두 개가 필요합니다. Wi-Fi로 실습하지 마세요. Visual Studio의 C++ 도구, Windows SDK와 MFC를 설치한 뒤 `build.ps1 -Configuration Release -Test`로 빌드합니다. 실행 PC에도 Npcap과 x86 Visual C++ 재배포 패키지가 필요합니다. Npcap SDK는 실행 환경을 대신하지 않습니다. 설치 후 프로젝트 폴더의 `run-router.cmd`를 실행하면 Npcap DLL 경로를 설정하고 `Release/ARP.exe`를 시작합니다.

1. 두 유선 NIC를 각 네트워크에 연결합니다. 강의자료의 실험 환경처럼 라우터 PC의 해당 NIC 두 개에서 TCP/IPv4 바인딩을 해제합니다. 인터넷 연결에 사용하는 어댑터를 잘못 변경하지 않도록 확인하세요.
2. 화면에서 Port 0과 Port 1에 서로 다른 NIC를 선택하고 각 포트의 IP 주소를 입력한 후 Select를 누릅니다. Port 0 MAC은 실제 장치의 MAC과 같아야 합니다. Port 1 MAC은 선택한 장치에서 자동으로 읽습니다.
3. Static Routing Table에 목적지 네트워크, 마스크, 포트와 U 또는 UG를 입력하고 Add를 누릅니다. UG일 때만 다음 라우터의 IP를 Gateway에 입력합니다. Default 경로는 목적지와 마스크를 모두 0.0.0.0으로 입력합니다. Delete로 선택된 경로를 지울 수 있습니다.
4. 양쪽 호스트에 IP, 마스크와 해당 라우터 포트 IP를 기본 게이트웨이로 설정합니다. 중간 상용 라우터가 있다면 돌아오는 경로도 설정되어 있어야 합니다. 양방향 ping 및 두 인터페이스에서 ICMP 캡처를 확인합니다.

라우팅은 가장 긴 마스크를 우선하고 U는 목적지, UG는 Gateway에 ARP를 요청합니다. 캐시가 비어 있으면 패킷을 잠시 보관합니다. TTL이 1 이하이거나 IP 헤더가 잘못된 패킷은 폐기합니다. 라우터 자신의 IP로 오는 ping이나 ICMP 오류 메시지 생성은 구현하지 않습니다. IPv4 패킷을 전달하며 IP 단편 재조립은 하지 않습니다.

화면의 ARP 캐시는 두 포트의 항목을 표시합니다. Proxy ARP와 수동 ARP/GARP 조작은 기존 Port 0 기능입니다. 테스트 토폴로지는 과제 자료의 예시 주소와 다르게 설계해야 합니다.
