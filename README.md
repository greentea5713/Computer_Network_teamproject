# ARP (Practice 6)

File Transfer 프로젝트의 CBaseLayer 계층 구조(NI, Ethernet, Base, LayerManager)를 재사용하여 ARP 프로그램을 구현했습니다.

> 브랜치: `arp` = ARP 프로그램(이 문서), `chat` = 기존 Ethernet 채팅/파일 전송 프로그램.

## 열기 및 빌드

- `MACFileTransfer.sln`을 Visual Studio에서 열고 **Debug / x86**으로 빌드합니다.
- Visual Studio 2022의 **MSVC v143 C++ 도구**, **C++ MFC (x86 및 x64)**, Windows SDK와 Npcap SDK가 필요합니다. Visual Studio Installer → 수정 → 개별 구성 요소에서 MFC를 추가할 수 있습니다.
- `dependencies\npcap-sdk-1.16`에 준비한 SDK를 우선 사용하며, 없으면 `C:\WpdPack`을 사용합니다. 다른 위치는 `NpcapSdkDir` MSBuild 속성으로 지정할 수 있습니다. SDK 공식 배포: https://npcap.com/#download
- 실행 PC에는 Npcap 드라이버가 필요합니다. **`run.cmd`를 더블 클릭**하면 설치된 Npcap DLL 경로를 적용하여 Debug x86 프로그램을 실행합니다. 실행 파일이 없으면 먼저 빌드합니다.
- Visual Studio에서 F5로 실행할 때도 프로젝트의 디버깅 환경에 Npcap 경로가 적용됩니다.
- x64 설정도 SDK의 `Lib\x64`를 사용하도록 구성했지만 실제 빌드 검증 대상은 Debug/Release x86입니다.

```powershell
# MFC가 없을 때 추가 설치 (Windows 관리자 권한 확인 필요)
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-mfc.ps1
# 앱 빌드
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
# 앱 빌드 및 네트워크를 사용하지 않는 프로토콜 검사
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
```

- `build.ps1`은 Visual Studio 2022 이상(18 Insiders 포함)의 v143 도구를 찾습니다.

## 계층 구조

```
        Application (Dialog)
               |
       ARP     IP          ← IP는 주소 해석을 ARP에 요청
          \   /
        Ethernet           ← EtherType 분기: 0x0806 → ARP(0), 0x0800 → IP(1)
            |
            NI             ← 캡처 필터: ether proto 0x0806
```

- 연결 문자열: `NI ( *Ethernet ( *ARP *IP ( +ARPDlg ) ) )`
- `ARPLayer`(`ARPLayer.h/.cpp`): ARP 메시지 송수신, 캐시 테이블.
- `IPLayer`(`IPLayer.h`): 자기 IP 보관, `Request`/`Resolve`로 ARP에 주소 해석 요청. IP 데이터그램 송수신은 라우팅 실습에서 확장합니다.
- `EthernetLayer`: 목적지 MAC을 지정해 송신합니다. 자기 MAC 목적지와, ARP에 한해 브로드캐스트를 수신합니다. 자기 송신 프레임은 버립니다.

## ARP 메시지와 동작

- 28바이트: hard type 1 / prot type 0x0800 / hard size 6 / prot size 4 / op (1 요청, 2 응답) / sender MAC·IP / target MAC·IP. Ethernet 최소 길이 60바이트로 패딩합니다.
- **요청**: 목적지 MAC 브로드캐스트, target MAC 0. 캐시에 없으면 `incomplete` 항목을 만듭니다.
- **요청 수신**: target IP가 내 IP이면 송신자를 캐시에 추가/갱신하고 sender/target을 바꿔(SWAPPING) 유니캐스트로 응답합니다. 내 IP가 아니면 응답하지 않고, 캐시에 이미 있는 송신자만 갱신합니다(RFC 826 merge).
- **응답 수신**: 송신자 매핑을 `complete`로 추가/갱신합니다.
- **캐시 테이블**: IP 주소를 키로 하는 해시 테이블(`std::unordered_map`, IPv4 4바이트를 32비트 정수로 묶어 해시)입니다. 화면에는 IP 순으로 정렬해 표시합니다.
- **캐시 만료**: complete 20분, incomplete 3분. NI 수신 스레드의 `OnIdle`과 UI 1초 타이머에서 정리합니다.
- 디자인에 로그 영역이 없어 최근 상태 메시지(요청/응답 송수신 등)는 창 제목 표시줄에 표시합니다.

## 실행 (Host A ↔ Host B)

1. 하단에서 어댑터를 선택합니다. MAC과 IP가 어댑터 값으로 채워지며 IP는 수정할 수 있습니다(예: A `168.188.129.63`, B `168.188.129.2`). **Select**를 누릅니다(다시 누르면 Reset).
2. Host A: ARP Cache 아래 IP 칸에 B의 IP를 입력하고 **Send**(또는 Enter). A의 캐시에 `incomplete` 항목이 생깁니다.
3. Host B는 요청을 받아 A를 캐시에 추가하고 응답합니다. A의 항목이 `complete`와 B의 MAC으로 바뀝니다.
4. **Item Delete**/**All Delete**로 캐시를 삭제합니다.
5. Wireshark 표시 필터: `arp`.

- PC의 실제 IP를 내 IP로 쓰면 Windows도 같은 요청에 응답하여 응답이 두 개 보일 수 있습니다. 실습 토폴로지처럼 사용하지 않는 IP를 지정하면 이 프로그램만 응답합니다.

## 검증 및 한계

- `build.ps1 -Test`: 실제 NIC 없이 모의 하위 계층으로 검사합니다(`tests/ProtocolTests.cpp`). 계층 연결, ARP 요청/응답 필드, 브로드캐스트 수신, SWAPPING, Ethernet 필터, 캐시 추가·갱신·만료·삭제, 해시 테이블 저장·검색. 2026-10-08 Debug x86 빌드와 검사 **73개 통과**.
- 실행 화면 배치를 과제 디자인과 대조했습니다. 실제 두 PC 사이의 ARP 교환은 아직 실행하지 않았습니다.
- ARP 요청 재전송은 하지 않습니다. 응답이 없으면 incomplete 항목이 3분 뒤 삭제됩니다.

## 보고서 제출 전

조원/학번, 실제 실습 일시·장소를 채우고 프로그램 결과 화면 및 Wireshark 캡처를 추가하세요. 미실행 상태에서 실제 성공한 실험처럼 기술하지 않았습니다.
