# NAT 실습 및 가산점 데모

## 직접 연결

```text
Host A ── Port 1 (Inside) [NAT] Port 2 (Outside) ── Host B
```

| 장비 | IPv4 | 마스크 | 기본 게이트웨이 |
| --- | --- | --- | --- |
| Host A | 192.168.42.10 | 255.255.255.0 | 192.168.42.1 |
| NAT Port 1 | 192.168.42.1 | 255.255.255.0 | 앱의 라우팅 테이블 사용 |
| NAT Port 2 | 10.42.0.2 | 255.255.255.0 | 앱의 라우팅 테이블 사용 |
| Host B | 10.42.0.10 | 255.255.255.0 | 비움 |

1. NAT PC의 실습용 유선 NIC 두 개에서 Windows 인터넷 프로토콜 버전 4(TCP/IPv4) 바인딩을 끈다. NAT 인터페이스 IP는 앱에 입력한다. Host A와 Host B의 NIC에서는 IPv4를 켜 두고 위 표처럼 설정한다. 실습 후 NAT PC의 IPv4 바인딩을 다시 켠다.
2. 앱 위쪽에서 Port 1에 Host A 쪽 NIC와 `192.168.42.1`, Port 2에 Host B 쪽 NIC와 `10.42.0.2`를 입력한다. `Select`로 시작한다.
3. Routing Table에 `192.168.42.0 / 255.255.255.0 / Port 1 / U`, `10.42.0.0 / 255.255.255.0 / Port 2 / U`를 추가한다. `U`에는 Gateway를 입력하지 않는다.
4. Host A에서 `ping 10.42.0.10`을 실행한다. Host B에서 ICMP Echo 요청을 허용하고, Wireshark의 양쪽 캡처에서 IP/Identifier가 바뀌었다가 응답에서 복원되는지 확인한다. NAT Table에서 ICMP 행이 생기고 마지막 패킷 후 약 60초 뒤 사라지는지도 확인한다.

NAT 테이블에 두 번의 ping을 나란히 표시하려면 첫 `ping`이 끝난 뒤 약 3초 기다렸다가, 첫 항목이 만료되기 전에 다시 실행한다. 한 번의 ping에서 이어지는 Echo Request는 한 외부 ID를 공유한다. 프로그램은 같은 내부 ID가 재사용된 경우에도 Sequence Number가 끊기거나 요청 사이의 간격이 2.5초를 넘으면 새 행을 만들며, 각 행은 마지막 사용으로부터 60초 뒤에 만료된다.

## TCP·UDP 가산점


**TCP 송신 NAPT** — Host B에서 `python -m http.server 8080 --bind 10.42.0.10`을 실행하고 Host A에서 `curl.exe http://10.42.0.10:8080/`을 실행한다. Host B 캡처에서 TCP 출발지 IP가 `10.42.0.2`이고 출발지 포트가 NAT Table의 외부 포트와 같은지 확인한다.

**TCP 포트포워딩** — Host A에서 `python -m http.server 8080 --bind 192.168.42.10`을 실행한다. 앱의 Port Forwarding에 `TCP / Outside 18080 / Inside IP 192.168.42.10 / Inside 8080`을 추가한다. Host B에서 `curl.exe http://10.42.0.2:18080/`을 실행한다. NAT Table에 `Forward` 행이 생기고 Host B 쪽 응답의 출발지 포트가 `18080`인지 확인한다.

**UDP 확인** — 서버를 띄울 호스트에서 아래 명령을 PowerShell에 입력한다. 송신 NAPT는 Host B에서 서버를 실행하고 Host A에서 클라이언트를 실행한다. UDP 포트포워딩은 Host A에서 서버를 실행하고 앱에 `UDP / Outside 17000 / Inside IP 192.168.42.10 / Inside 7000`을 추가한 뒤 Host B에서 클라이언트를 실행한다.

```powershell
# UDP echo server, 종료할 때 Ctrl+C
$server = [System.Net.Sockets.UdpClient]::new(7000)
$peer = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
try {
    while ($true) {
        $data = $server.Receive([ref]$peer)
        Write-Host $peer ([Text.Encoding]::UTF8.GetString($data))
        [void]$server.Send($data, $data.Length, $peer)
    }
} finally { $server.Dispose() }
```

```powershell
$target = '10.42.0.10'; $port = 7000  # 포트포워딩 때는 10.42.0.2 및 17000
$client = [System.Net.Sockets.UdpClient]::new()
try {
    $client.Client.ReceiveTimeout = 3000
    $data = [Text.Encoding]::UTF8.GetBytes('NAT UDP echo')
    [void]$client.Send($data, $data.Length, $target, $port)
    $peer = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $reply = $client.Receive([ref]$peer)
    Write-Host $peer ([Text.Encoding]::UTF8.GetString($reply))
} finally { $client.Dispose() }
```

테스트 중에는 서버를 띄운 호스트에서 해당 인바운드 프로토콜/포트를 허용한다. 서버와 클라이언트는 실습망에서 실행한다. Windows에서 접근이 막히면 방화벽 규칙의 네트워크 프로필과 원격 서브넷을 확인한다.

## Cisco를 거치는 데모

Host B가 Cisco 뒤에 있는 데모에서는 조교가 지정한 IP를 사용한다. 예를 들어 NAT Port 2가 `10.42.0.2/24`, Cisco의 NAT 쪽 포트가 `10.42.0.1/24`, Host B의 네트워크가 `172.16.42.0/24`이면 NAT 테이블이 아닌 **Routing Table**에 `172.16.42.0 / 255.255.255.0 / 10.42.0.1 / Port 2 / UG`를 추가한다. Host B는 자기 LAN의 Cisco IP를 게이트웨이로 사용하고, Cisco에는 Host A의 내부 네트워크에 대한 경로 없이 NAT 외부 IP까지 돌아오는 경로가 있어야 한다. TCP/UDP 및 포트포워딩 시험에 쓰는 IP도 이 구성에 맞춰 바꾼다.
