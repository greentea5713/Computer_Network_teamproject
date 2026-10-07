# MACFileTransfer Ethernet 채팅 및 파일 전송

기존 IPC 프로젝트를 File Transfer 1 자료의 CBaseLayer 계층 구조로 확장했습니다.

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

2026-10-01 이 PC에서 Npcap SDK 1.16, v143, MFC 준비를 완료했습니다. 설치 실패 원인은 Windows hosts 파일의 오래된 `192.229.232.200 download.visualstudio.microsoft.com` 항목이었으며, 원본을 백업한 뒤 해당 항목을 주석 처리했습니다. Debug x86 앱 빌드 및 기존 프로토콜 검사 **522개 통과**를 확인했습니다. 실제 두 PC 사이의 전송은 별도 확인이 필요합니다.

## 두 PC 간 전송 실행

1. 유선으로 연결한 PC 두 대에서 같은 수정본을 사용합니다.
2. 각 PC에서 실제 Ethernet 어댑터를 선택합니다. Packet32 OID로 MAC을 조회합니다. 조회 실패 시 해당 NIC의 실제 MAC을 수동 입력합니다.
3. 상대 PC 유선 어댑터의 MAC을 목적지에 입력한 후 주소 설정을 누릅니다.
4. 채팅 입력 후 전송 또는 Enter를 누릅니다. Shift+Enter는 줄바꿈입니다.
5. 파일 선택 후 파일 전송을 누릅니다. 파일 전송 중에도 채팅할 수 있습니다.
   - 하단 송신/수신 진행 막대에 퍼센트와 완료 또는 중단/실패 상태가 표시됩니다. 새 전송이 시작되면 해당 막대가 0%로 초기화됩니다.
   - 송신 막대의 완료는 로컬 송신 완료입니다. 상대 PC의 수신 완료도 확인하세요.
6. 수신 파일은 실행 파일 옆 `Received` 폴더에 `received_시각_번호_원본이름`으로 저장됩니다. 쓰기 권한이 있는 폴더에서 실행하세요.
7. Wireshark 표시 필터: `eth.type == 0x2080 || eth.type == 0x2090`.
8. 종료 또는 연결 해제 시 작업을 중단하고 스레드를 합류한 후 장치를 닫습니다.

## 주요 변경

- 기존 `CFileLayer`는 참고용 소스로 보존하되 빌드와 계층 연결에서 제외했습니다.
- `CNILayer`를 추가하여 실제 Npcap 프레임 송수신과 `UINT ReadingThread(LPDWORD)` 수신 스레드를 구현했습니다.
- `CEthernetLayer`는 14바이트 헤더, 목적지/자기 송신 필터와 EtherType 분기를 담당합니다.
- `CChatAppLayer`는 UTF-8 바이트 단편화와 재조립, `CFileAppLayer`는 스트리밍 파일 송수신을 담당합니다.
- `CLayerManager`의 연결 문자열 방식을 유지하고 범위 및 구문 검사를 보완했습니다.
- Dialog가 계층 객체의 수명을 소유합니다. UI 갱신은 큐와 WM_APP 메시지를 사용합니다.

## 프로토콜 결정

자료의 p.29(1456)와 p.26/28/30(1496)이 충돌하여 실제 Ethernet MTU 1500 - 채팅 헤더 4 = **1496**을 채택했습니다. IP/TCP 헤더는 없습니다.

자료 p.33을 우선하여 채팅 type **0=전체 길이 메타데이터, 1=중간 데이터, 2=마지막 데이터**로 정의했습니다. 짧은 채팅도 메타데이터+마지막 데이터 두 프레임을 사용합니다. p.28의 비단편 0/첫 조각 1 예시와는 다른 결정입니다. 첫 프레임에 채팅 본문은 넣지 않습니다.

- Ethernet: 목적지 MAC 6 / 출발지 MAC 6 / type 2. 채팅 0x2080, 파일 0x2090.
- 채팅: totlen 2 / type 1 / unused 1 / data 최대 1496. 숫자는 네트워크 바이트 순서입니다.
- 채팅 길이 ≤65535: unused=0, 모든 조각 totlen=전체 UTF-8 길이.
- 채팅 길이 >65535: unused=1, totlen=0xFFFF. 첫 프레임 data에 64비트 전체 길이를 담습니다. 이후 조각은 일반 데이터입니다.
- 파일: totlen 4 / type 2 / message type 1 / unused 1 / sequence 4 / data 최대 1488.
- 파일 type: 0=메타데이터, 1=본문, 2=종료. message type=0(바이너리).
- 파일 ≤4GiB-1: unused=0. 메타데이터 data=UTF-8 원본 파일명+NUL.
- 파일 >4GiB-1: unused=1, totlen=0xFFFFFFFF. 메타데이터 data=64비트 전체 크기+UTF-8 파일명+NUL.
- 파일 seq: 메타데이터=0, 본문=1부터, 종료=다음 번호. wrap은 허용하지 않습니다.
- 마지막 본문 길이는 전체 크기와 이미 받은 바이트 수로 계산해 Ethernet 패딩을 제외합니다.
- 긴 길이 확장은 본 구현의 추가 규약입니다. 다른 조의 프로그램과 사용하려면 이 규약까지 합의해야 합니다.

## 검증 및 한계

- Debug/Release x86 컴파일 및 링크 확인.
- `MACFileTransfer/ProtocolTests.vcxproj`는 실제 NIC를 열지 않는 네이티브 C++ 단위 검사입니다. `tests/bin/ProtocolTests.exe`가 모의 하위 계층을 통해 프레임을 검사합니다.
- 검사 소스: `tests/ProtocolTests.cpp`. 결과 로그는 `../output/verification`에 있습니다.
- 진행 막대 추가 후 Debug x86 빌드 및 프로토콜 검사 575개 통과. 빈 파일, 진행률 초기화·증가·완료, 송수신 실패 상태도 검사했습니다. 실행 화면에서 하단 막대 배치를 확인했습니다.
- 실제 두 PC, 드라이버, Wireshark, 동시 채팅/파일 송수신은 아직 미실행입니다.
- 송신 완료는 로컬 pcap_sendpacket 성공이며 상대 수신 확인이 아닙니다. ACK/ARQ/재전송/흐름 제어는 구현하지 않았습니다. IPC의 Windows ACK와 2초 타이머를 실제 LAN 수신 확인으로 재사용하지 않습니다.
- 채팅 원형 헤더에는 조각 순번이 없습니다. 누락과 최종 길이 오류 일부는 검출하지만 동일 길이 조각의 재정렬/중복 대체를 완전히 검출하지 못합니다.
- 파일 순서와 크기를 검사하나 전송 전체 체크섬은 없습니다. 실제 검증 때 원본과 수신 파일의 SHA-256을 비교하세요.
- UI/UTF-8 변환의 길이는 INT_MAX, 사용 가능한 메모리·디스크에 제한됩니다. 파일은 32비트 순번 범위(최대 1488×(2^32-2)바이트) 이내입니다. 수학적인 무제한 전송은 아닙니다.
- 동시 수신 상태는 채팅 32개, 파일 16개이며 출발지·목적지 MAC 쌍별 한 개씩 유지합니다. 같은 MAC 쌍에서 새 첫 프레임을 받으면 기존 미완료 전송을 교체합니다.
- 30초간 진전 없는 수신은 폐기합니다. UI에는 최근 1000행이 남습니다. 파일 전송은 조각마다 1ms 간격을 두지만 혼잡에 대한 신뢰성 보장은 없습니다.
- 파일 생성은 CREATE_NEW, 경로문자 제거, 고유 이름, `.part` 저장 후 완료 시 이름 변경으로 처리합니다. 첫 메타데이터를 받으면 전체 파일 크기로 공간을 확보하고 포인터를 처음으로 되돌린 뒤 조각을 기록합니다. 실패·취소 파일은 삭제합니다.
- 802.1Q VLAN, 무선 모니터 모드, 라우터를 거치는 IP 통신은 대상으로 하지 않습니다.

## ARP (Practice 6)

### 계층 구조

```
            Application (Dialog)
                   |
ChatApp  FileApp  ARP   IP   ← IP는 주소 해석을 ARP에 요청
   \       |      |    /
          Ethernet          ← EtherType 분기: 0x2080 / 0x2090 / 0x0806 / 0x0800
             |
             NI
```

- `NILayer`, `EthernetLayer`, `BaseLayer`, `LayerManager`는 그대로 재사용하고, `ARPLayer`(`ARPLayer.h/.cpp`)와 `IPLayer`(`IPLayer.h`)를 새로 추가했습니다.
- 연결 문자열: `NI ( *Ethernet ( *ChatApp ( +ChatDlg ) *FileApp ( +ChatDlg ) *ARP *IP ( +ChatDlg ) ) )`
- `EthernetLayer`는 Frame Type 0x0806을 ARP 레이어로 올립니다. 목적지가 `ff:ff:ff:ff:ff:ff`인 브로드캐스트는 ARP 프레임만 받습니다. 채팅/파일은 기존처럼 자기 MAC 목적지만 받습니다.
- ARP 요청/응답은 목적지 MAC을 지정하는 `SendPacket(data, len, type, destination)`으로 보냅니다.
- NI 캡처 필터에 `ether proto 0x0806`을 추가했습니다.
- `IPLayer`는 이번 실습에서 자기 IP를 보관하고 `Request`/`Resolve`로 ARP에 주소 해석을 요청합니다. IP 데이터그램 송수신은 라우팅 실습에서 확장합니다.

### ARP 메시지와 동작

- 28바이트: hard type 1 / prot type 0x0800 / hard size 6 / prot size 4 / op (1 요청, 2 응답) / sender MAC·IP / target MAC·IP. Ethernet 최소 길이 60바이트로 패딩합니다.
- 요청: 목적지 MAC 브로드캐스트, target MAC은 0으로 채움. 캐시에 없으면 Incomplete 항목을 만듭니다.
- 요청 수신: target IP가 내 IP이면 송신자를 캐시에 추가/갱신하고 sender/target을 바꿔(SWAPPING) 송신자에게 유니캐스트로 응답합니다. 내 IP가 아니면 응답하지 않습니다. 캐시에 이미 있는 송신자만 갱신합니다(RFC 826 merge).
- 응답 수신: 송신자 매핑을 Complete로 추가/갱신합니다.
- 캐시 만료: Complete 20분, Incomplete 3분. NI 수신 스레드의 `OnIdle`과 UI 1초 타이머에서 정리합니다.
- 송신자 IP가 내 IP인데 MAC이 다르면 "IP 주소 충돌"을 표시합니다.

### 실행 (Host A ↔ Host B)

1. 두 PC에서 어댑터를 선택합니다. MAC과 내 IP가 어댑터 값으로 채워지며 IP는 수정할 수 있습니다(예: A `168.188.129.63`, B `168.188.129.2`). **주소 설정**을 누릅니다. 목적지 MAC은 비워 두어도 됩니다.
2. Host A의 ARP Cache 영역에 B의 IP를 입력하고 **Send**(또는 Enter)를 누릅니다. A의 캐시에 `Incomplete` 항목이 생깁니다.
3. Host B는 요청을 받아 A를 캐시에 추가하고 응답합니다. A의 항목이 `Complete`와 B의 MAC으로 바뀝니다.
4. **Item Delete**/**All Delete**로 캐시를 삭제합니다. 캐시 항목을 더블클릭하면 채팅/파일 목적지 MAC에 입력됩니다.
5. Wireshark 표시 필터: `arp`.

- PC의 실제 IP를 내 IP로 쓰면 Windows도 같은 요청에 응답하여 응답이 두 개 보일 수 있습니다. 실습 토폴로지처럼 사용하지 않는 IP를 지정하면 이 프로그램만 응답합니다.
- 프로토콜 검사(`build.ps1 -Test`)에 ARP 요청/응답 필드, 브로드캐스트 수신, SWAPPING, 캐시 추가·갱신·만료·삭제 검사를 추가했습니다. 2026-10-07 Debug x86 빌드와 검사 **617개 통과**를 확인했습니다. 실제 두 PC 사이의 ARP 교환은 아직 실행하지 않았습니다.
- `build.ps1`은 Visual Studio 2022 이상(18 Insiders 포함)의 v143 도구를 찾습니다.

## 보고서 제출 전

조원/학번, 실제 실습 일시·장소를 채우고 프로그램 결과 화면 및 Wireshark 캡처를 추가하세요. 미실행 상태에서 실제 성공한 실험처럼 기술하지 않았습니다.
