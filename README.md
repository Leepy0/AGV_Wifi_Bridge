# AGV Wi-Fi Bridge (ESP32-S3 + BOB-11189)

ZigBee 통신보드 대체용 브리지입니다.

```
RFID 리더 ──RS232──┐                              ┌── UDP 50000 ── 서버(Gateway) ── ACS
                   ├─ BOB-11189 ─ ESP32-S3 ─ Wi-Fi ┤
메인보드 J4 ─RS232─┘   (MAX3232)                   └── UDP 50001 브로드캐스트 (RFID)
```

- **메인보드 J4 ↔ 서버**: UART 프레임을 그대로 UDP 로 중계합니다. ACK·재전송·MSG ID 는 기존처럼 메인보드가 관리합니다.
- **RFID 리더 → 메인보드**: 기존 통신보드처럼 바이패스합니다. Wi-Fi 연결과 무관하게 동작합니다.
- **RFID 리더 → 네트워크**: 브로드캐스트(기본값) 또는 지정 IP 로 송신합니다.
- 메인보드로 가는 데이터(RFID, ACS 하향)는 송신 태스크 하나가 **프레임 단위로** 내보냅니다. 두 데이터가 섞이지 않습니다.

## 개발 환경

- ESP-IDF **v6.1** (v5.5 도 빌드 가능하도록 작성)
- VS Code + ESP-IDF 확장 권장. CLI 만 쓴다면 `idf.py` 로 충분합니다.

```bash
idf.py set-target esp32s3
idf.py menuconfig        # AGV Wi-Fi Bridge 메뉴: 기본 SSID/서버 IP/핀/프레임 간격
idf.py -p COM5 flash monitor
```

## 호기별 설정 (재빌드 없이)

USB 시리얼 콘솔(115200)에서 입력합니다. DevKitC 기준 **UART 포트** 쪽 USB 입니다.

```
bridge> set agv 7
bridge> set ssid AGV_WIFI
bridge> set pass mypassword
bridge> set ip 192.168.10.107      # 현장은 고정 IP 권장 (DHCP 는 재연결마다 수백 ms~1초 추가)
bridge> set gw 192.168.10.1
bridge> set server 192.168.10.10
bridge> set rfid_baud 9600         # RFID 리더 실제 보드레이트 확인 필요
bridge> save
bridge> reboot
```

| 명령 | 설명 |
|---|---|
| `cfg` | 현재 설정 표시 |
| `set <key> <value>` | 편집 사본만 변경 — 동작 중 설정은 안 바뀜. 인자 없이 `set` 입력 시 키 목록 |
| `save` / `reboot` | NVS 저장 / 재부팅 (재부팅 후 적용) |
| `defaults` | 저장된 설정 삭제 → menuconfig 기본값 사용 |
| `stat` | Wi-Fi(RSSI, BSSID), 송수신·오류 카운터 (`tx_offline`: Wi-Fi 끊김 중 폐기, `tx_drop`: 메인보드 송신 폐기) |
| `mon 1` / `mon 0` | 모든 프레임 hex 덤프 (벤치 확인용) |

## 기본 핀

| 기능 | ESP32-S3 | BOB-11189 |
|---|---|---|
| 메인보드 UART1 TX | GPIO17 | T1IN |
| 메인보드 UART1 RX | GPIO18 | R1OUT |
| RFID UART2 TX | GPIO4 | T2IN |
| RFID UART2 RX | GPIO5 | R2OUT |
| 전원 | 3V3 | 3V-5.5V |
| GND | GND | GND |

## 프레임 구분 방식

- **메인보드(J4): ETX 기준.** ETX(0x03)를 받는 즉시 프레임을 서버로 보냅니다. 마지막 바이트 이후 약 0.2ms 안에 처리됩니다.
  - 연달아 온 두 프레임도 ETX 로 나뉘어 각각 전송됩니다.
  - ETX 없이 3ms 넘게 끊기면 받은 데이터를 그대로 보내고 `stat` 의 `no_etx` 를 올립니다.
  - 체크섬 문자가 0x03 과 겹치면 프레임이 잘못 잘립니다. 이런 경우 `menuconfig > Framing` 에서 ETX 모드를 끄면 무수신 3ms 방식으로 돌아갑니다.
- **RFID 리더: 무수신 5ms 기준.** 리더 데이터 형식과 무관하게 동작합니다.
- **메인보드로 송신:** 프레임 사이 간격 0(연속 송신)이 기본입니다. 메인보드가 STX/ETX 로 구분한다는 전제이며, 필요하면 `Min idle gap` 을 늘리세요.

## UDP 패킷 규격

헤더 10바이트 + payload, 리틀 엔디언입니다.

| offset | 크기 | 내용 |
|---|---|---|
| 0 | 2 | magic `A5 5A` |
| 2 | 1 | ver = 1 |
| 3 | 1 | type |
| 4 | 1 | AGV ID |
| 5 | 1 | flags (0) |
| 6 | 2 | seq (타입별 순번 — 누락 감지) |
| 8 | 2 | payload 길이 |

| type | 방향 | payload |
|---|---|---|
| `0x01` MAIN_UP | ESP → 서버 | 메인보드 UART 원본 |
| `0x02` MAIN_DOWN | 서버 → ESP | 메인보드로 보낼 UART 원본 (AGV ID 일치 시만 처리) |
| `0x03` RFID | ESP → rfid_dst | RFID 리더 원본 |
| `0x10` STATUS | ESP → 서버 | JSON: rssi, ch, bssid, disc(끊김 횟수), 카운터 (기본 1초 주기) |
| `0x20` PING | 서버 → ESP | 임의 (타임스탬프) |
| `0x21` PONG | ESP → 서버 | PING 의 seq·payload 그대로 |

- 하향 패킷은 설정된 `server` IP 에서 온 것만 받습니다.
- 서버는 ESP 가 보낸 패킷의 송신 endpoint(IP:50000)를 기억했다가 그곳으로 하향 송신하면 됩니다.
- Wi-Fi 가 끊긴 동안의 상향 프레임은 **버퍼링하지 않고 폐기**합니다. 지난 교통제어 패킷이 늦게 도착하는 것보다 메인보드 재전송이 안전합니다. seq 는 계속 증가하므로 서버에서 누락 구간이 보입니다.

## 벤치 테스트 순서

1. **RS232 루프백**: BOB 의 T1OUT–R1IN 을 점퍼로 연결합니다. `mon 1` 상태에서 서버가 보낸 MAIN_DOWN 이 그대로 MAIN_UP 으로 돌아오는지 확인합니다.
2. **메인보드 대역 PC**: USB-RS232 어댑터를 채널 1 에 연결하고, 터미널(RealTerm 등, 115200)로 `STX…ETX` 프레임을 송수신합니다.
3. **RFID 리더 연결**: 태그를 읽혀 `RFID>` 덤프와 서버 수신을 확인합니다. 보드레이트가 안 맞으면 깨진 데이터로 보이니 `set rfid_baud` 로 맞춥니다.
4. **지연 측정**: `tools/BridgeTest.cs` 에서 `ping 7 100` 을 실행합니다. 교통제어 0.5초 목표 대비 여유를 확인하세요.

```bash
cd tools
dotnet run BridgeTest.cs     # .NET 10, 명령: list | send <agv> <data> | ping <agv> [n] | hb | quit
```

## 적용 전 확인·다음 단계

- AGV 장착 시에는 BOB-11189 대신 절연형(TD301D232H ×2)을 사용합니다. GND 전위차·모터 노이즈 대책입니다.
- 다중 AP 구간에서는 `menuconfig > Component config > Wi-Fi > Advanced support for Wi-Fi Roaming` 을 검토하세요(RSSI 기반 선제 로밍).
- 400대 운용 시 OTA 파티션 구성과 서버 측 Gateway(ACS 소켓 연동)가 필요합니다.
