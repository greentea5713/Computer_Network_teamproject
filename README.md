# Static Router

Windows에서 Npcap을 이용해 Ethernet 인터페이스 두 개 사이에서 IPv4 패킷을 전달하는 MFC 실습 프로젝트입니다. 기존 ARP 구현을 활용하며, UI에서 포트 IP와 정적 경로를 입력합니다.

- U 직접 연결 경로와 UG 게이트웨이 경로, 최장 접두사 검색
- 포트별 ARP 캐시 및 다음 홉 주소 해석
- IPv4 TTL 감소와 헤더 체크섬 재계산
- 라우터 자신의 IP로 향하는 ping 응답 및 ICMP 오류 생성은 미구현

## 실행

실행 PC에는 [Npcap](https://npcap.com/#download)과 **x86 Visual C++ 재배포 패키지**, 서로 다른 유선 네트워크 인터페이스 두 개가 필요합니다. `Release/ARP.exe`가 없다면 아래 방법으로 먼저 빌드하세요. Npcap을 설치한 후 `run-router.cmd`를 실행하면 설치된 Npcap의 DLL 경로를 설정하고 라우터를 시작합니다.

저장소에 포함된 Npcap SDK는 빌드에 사용합니다. 실행에 필요한 Npcap 드라이버와 DLL은 Npcap 설치 프로그램이 설치합니다. 유선 포트 연결, 주소 입력과 ping 시험 방법은 [라우터 사용 안내](ROUTER_GUIDE.md)를 참고하세요.

## 빌드

1. Visual Studio 2022 또는 2026에서 C++ 데스크톱 개발 도구, MFC 및 Windows SDK를 설치합니다.
2. 저장소에 포함된 `dependencies/npcap-sdk-1.16`의 다음 파일을 확인합니다. 이 파일들이 없는 사본에서는 [Npcap 공식 배포 페이지](https://npcap.com/dist/)에서 `npcap-sdk-1.16.zip`을 내려받아 해당 경로에 압축을 풉니다.
   - `dependencies/npcap-sdk-1.16/Include/pcap.h`
   - `dependencies/npcap-sdk-1.16/Lib/wpcap.lib`
   - `dependencies/npcap-sdk-1.16/Lib/Packet.lib`
3. PowerShell에서 `./build.ps1 -Configuration Release -Test`를 실행합니다. 빌드 성공 시 `Release/ARP.exe`가 생성됩니다.

다른 위치에 SDK가 있다면 PowerShell에서 `$env:NpcapSdkDir = 'SDK 경로'`를 설정하고 빌드할 수도 있습니다. 기본 프로젝트 구성은 Win32(x86)입니다.

## 확인 상태

프로토콜 자동 검사는 실제 NIC를 열지 않습니다. 실기기 환경에서는 유선 인터페이스 두 개와 양쪽 호스트를 연결해 양방향 ping 및 Wireshark 캡처를 별도로 확인해야 합니다.

`Release` 폴더의 실행 파일과 DLL, 빌드 중간 파일은 Git에 포함되지 않습니다.
