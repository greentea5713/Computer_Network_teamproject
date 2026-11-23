# NAT Router

Computer Network Practice 12 과제용 MFC/Npcap IPv4 NAT 프로그램. Router II 프로젝트를 바탕으로 ICMP/TCP/UDP NAPT와 TCP/UDP 포트포워딩을 구현했다.

- **실행** — ZIP을 풀고 `run-router.cmd` 실행. 노트북에 Npcap 런타임과 Visual C++ 재배포 패키지 **x86**이 필요하다. 어댑터 접근이 제한된 경우 관리자 권한으로 실행한다.
- **빌드** — Visual Studio에서 `ARP.sln` 열기. Desktop development with C++, MFC 및 Windows SDK가 필요하다. Npcap SDK 1.16을 이 폴더의 `dependencies/npcap-sdk-1.16`에 풀거나 MSBuild의 `NpcapSdkDir`로 위치를 지정한다. 같은 상위 폴더에 있는 기존 프로젝트의 `dependencies`도 자동으로 찾는다. `build.ps1 -Configuration Release -Test`는 x86 실행 파일을 빌드하고 패킷 테스트를 실행한다.
- **실습** — [ROUTER_GUIDE.md](ROUTER_GUIDE.md)

변환 대상은 조각나지 않은 IPv4 ICMP Echo, TCP, UDP다. IPv4 조각 재조립, ICMP 오류 메시지 변환, IPv6, 헤어핀 NAT와 라우터 자체 IP의 ICMP Echo 응답은 지원 범위에 포함하지 않았다. 동적 엔트리는 ICMP/UDP 60초, TCP 600초 동안 사용하지 않으면 만료된다. 화면의 포트포워딩 규칙은 앱을 종료할 때까지 유지된다.
