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
- **설정**: USB 콘솔(유선) 외에 서버 원격 명령(UDP)이나 휴대폰 블루투스(BLE)로도 바꿀 수 있습니다.

## 빌드 없이 굽기 (GitHub Actions 빌드 결과)

푸시할 때마다 Actions 가 ESP-IDF v6.1 로 빌드합니다. `v*` 태그를 푸시하면 **Releases** 에 zip 이 첨부됩니다.

1. Releases(또는 Actions 아티팩트)에서 `agv_wifi_bridge_<버전>.zip` 을 받아 풉니다.
2. 보드를 USB(UART 포트 쪽)로 연결하고 실행합니다.
   ```
   flash.bat COM5
   ```
   Arduino IDE 가 깔려 있으면 ESP32 코어에 포함된 `esptool.exe` 를 자동으로 찾아 씁니다. 없으면 `pip install esptool`.
   Arduino IDE 자체로는 미리 빌드한 .bin 을 올릴 수 없으니(스케치 컴파일 전용) 이 스크립트나 Espressif Flash Download Tool 을 씁니다.
3. 처음 한 번만 USB 로 굽고, 이후 버전은 OTA 로 올립니다 (아래).

> 전체 이미지(merged.bin)는 저장된 설정(NVS)까지 덮어씁니다. 설정한 호기를 USB 로 다시 구울 때는 `write_flash 0xf000 ota_data_initial.bin 0x20000 agv_wifi_bridge.bin` 으로 앱만 굽습니다.

**포트 확인·연결 실패 대처·호기 설정·동작 확인까지 단계별 절차는 [docs/FLASH_GUIDE.md](docs/FLASH_GUIDE.md)** (zip 에도 포함)를 보세요.

zip 내용: `agv_wifi_bridge_merged.bin`(전체 이미지, 0x0), `agv_wifi_bridge.bin`(OTA 용 앱만), bootloader / partition-table / ota_data 개별 파일, `flash_args`.

## 개발 환경

- ESP-IDF **v6.1** (v5.5 도 빌드 가능하도록 작성)
- VS Code + ESP-IDF 확장 권장. CLI 만 쓴다면 `idf.py` 로 충분합니다.

```bash
idf.py set-target esp32s3
idf.py menuconfig        # AGV Wi-Fi Bridge 메뉴: 기본 SSID/서버 IP/핀/프레임 간격/무선 설정
idf.py -p COM5 flash monitor
```

## OTA 무선 업데이트

파티션은 `ota_0` / `ota_1` 두 슬롯(각 1.9MB)이며 롤백이 켜져 있습니다.

- 서버가 `OTA_START`(0x30) 패킷에 URL 을 담아 보내면 ESP 가 HTTP 로 `agv_wifi_bridge.bin` 을 받아 빈 슬롯에 기록하고 재부팅합니다.
- 진행 상황은 `OTA_RESULT`(0x31) JSON 으로 옵니다: `start` → `progress`(10% 단위) → `done`(재부팅) / `fail`(err) / `busy`.
- 새 펌웨어로 부팅하면 **서버에서 하향 패킷(PING 등)을 한 번 받을 때** 확정됩니다. 180초 안에 서버 통신이 없으면 이전 슬롯으로 자동 롤백합니다. 상태는 heartbeat 의 `ota` 필드(`pending`/`valid`/`busy`)와 `part` 로 확인합니다.
- 프로젝트 이름이 다른 이미지는 거부합니다.
- 업데이트 중에도 UART 중계는 계속되지만 플래시 기록 중 수 ms 지연이 생기고, 재부팅으로 1~2초 끊깁니다. **AGV 정지 상태에서만** 보내고, 다수 호기는 순차로 진행하세요.

벤치: `BridgeTest` 에서 `ota 7 build\agv_wifi_bridge.bin` — PC 가 8070 포트로 파일을 서비스하고 URL 을 전달합니다. 콘솔에서는 `ota http://...` / `ota confirm` / `ota`(상태).

## 호기별 설정 (재빌드 없이)

세 방법 모두 같은 키(`agv`, `ssid`, `pass`, `ip` …)를 쓰고, **저장 후 재부팅해야 적용**됩니다.

| 방법 | 쓰는 경우 | 연결 |
|---|---|---|
| USB 시리얼 콘솔 | 벤치, 처음 굽을 때 | USB 케이블 |
| 서버 원격 설정 (UDP) | 운용 중인 호기의 IP·서버 주소·보드레이트 등 변경 | 기존 Wi-Fi |
| 휴대폰 BLE | 처음 설치, SSID/비밀번호가 틀려 Wi-Fi 에 못 붙을 때 | 블루투스(BLE) 터미널 앱 |

### 1. USB 시리얼 콘솔 (유선)

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
| `ota` / `ota <url>` / `ota confirm` | 펌웨어 상태 / 수동 OTA / 롤백 대기 수동 확정 |
| `setup` / `setup on` / `setup off` | 무선 설정 모드(BLE) 상태 / 열기 / 닫기 |

### 2. 서버 원격 설정 (무선, UDP)

서버가 `CFG_CMD`(0x40) 패킷에 콘솔과 같은 명령 한 줄을 담아 보내면 결과가 `CFG_RESULT`(0x41) 텍스트로 돌아옵니다. 설정된 `server` IP 에서 온, AGV ID 가 일치하는 패킷만 처리합니다.

```
cfg 7                          # (BridgeTest) 현재 설정 — 비밀번호는 길이만 표시
cfg 7 set ip 192.168.10.107
cfg 7 set ssid AGV WIFI 2F     # 값은 줄 끝까지 (공백 있는 SSID 가능)
cfg 7 save
cfg 7 reboot
```

명령: `cfg` | `set <key> <value>` | `save` | `reboot` | `stat` | `setup on|off` | `help` (`defaults` 는 원격에서 불가)

- **원격 저장은 확인 대기 상태로 저장됩니다.** 재부팅 후 180초 안에 서버에서 하향 패킷(PING 등)을 받지 못하면 저장 직전 설정으로 되돌리고 다시 재부팅합니다. SSID·IP·서버 주소를 잘못 넣어도 호기와 연결이 끊긴 채로 남지 않습니다(OTA 롤백과 같은 기준).
- 서버 IP 를 바꾼 경우에는 **새 서버**가 180초 안에 패킷을 보내야 확정됩니다.
- 상태는 `cfg` 출력의 `state=` 와 heartbeat 의 `cfg`(`saved` / `trial` / `default`)로 봅니다.
- 콘솔이나 휴대폰(BLE)에서 저장하면 현장 확인으로 보고 대기 중인 복구를 취소합니다.
- 인증은 서버 IP 필터뿐입니다(`OTA_START` 와 같은 수준). 사내망 전제입니다.

### 3. 휴대폰 BLE (무선, 현장)

휴대폰 블루투스(BLE)로 ESP 에 접속해 콘솔과 같은 명령을 입력합니다. 회사 Wi-Fi 보안 정책과 관계없이 쓸 수 있고, 그동안에도 브리지 중계는 계속 동작합니다.

1. 설정 모드 열기 — 다음 중 하나
   - 저장된 설정이 없을 때(처음 설치, `defaults` 후) 부팅하면 자동으로 열림
   - **BOOT 버튼(GPIO0) 3초** 누름
   - 콘솔 `setup on` / 서버 `cfg 7 setup on`
2. 휴대폰 BLE 터미널 앱으로 `AGVBR-<호기>-<MAC 끝 4자리>` 에 연결합니다.
   - Android: **Serial Bluetooth Terminal** → 메뉴 Devices → **Bluetooth LE** 탭 → SCAN
   - iPhone: **Bluefruit Connect** → 연결 → UART (또는 nRF Toolbox → UART)
   - 앱의 블루투스 목록(설정 → 블루투스)에서 페어링하는 방식이 아닙니다. 반드시 앱 안에서 스캔해 연결합니다.
3. 아래처럼 입력합니다.
   ```
   login agvsetup
   set agv 7
   set ssid AGV_WIFI
   set pass mypassword
   set ip 192.168.10.107
   set gw 192.168.10.1
   set server 192.168.10.10
   save
   reboot
   ```
4. 재부팅 후에도 BLE 가 다시 열려 있으므로, 다시 연결해 `login` → `stat` 으로 `wifi: UP` 을 확인합니다.

- 비밀번호 기본값은 `agvsetup` 입니다. menuconfig 에서 바꾸세요. 새로 연결할 때마다 `login` 이 필요합니다.
- 명령: `cfg` · `set` · `save` · `reboot` · `stat` · `defaults` · `setup off` · `help`. 현장 저장이라 즉시 확정됩니다.
- 휴대폰 연결 없이 Wi-Fi 연결 상태로 5분이 지나면 설정 모드가 자동으로 닫힙니다. Wi-Fi 미연결 상태에서는 유일한 접근 경로이므로 닫지 않습니다.
- **BLE 는 설정 모드 동안만 켭니다.** Wi-Fi 와 무선부(2.4GHz)를 나눠 쓰기 때문에, 휴대폰이 연결된 동안에는 Wi-Fi 지연이 늘 수 있습니다. 설정 모드가 닫히면 BT 컨트롤러를 완전히 내려 평상시 성능에는 영향이 없습니다. 운용 중 호기는 **AGV 정지 상태에서** 여세요.
- ESP32-S3 는 BLE 만 지원합니다. 클래식 블루투스(SPP) 전용 앱으로는 보이지 않습니다.

**Wi-Fi 설정 AP + 웹 페이지 (선택, 기본 꺼짐)** — `menuconfig > Wireless setup > Wi-Fi SoftAP` 을 켜면 설정 모드에서 AP(`http://192.168.4.1`)도 함께 띄웁니다. 사내 무선 보안(WIPS)이 비인가 AP 를 차단하거나 경보를 내는 현장에서는 쓰지 마세요.

`menuconfig > AGV Wi-Fi Bridge > Wireless setup` 에서 BLE 비밀번호, 자동 종료 시간, 첫 부팅 자동 열기, 버튼 GPIO(-1 = 사용 안 함), SoftAP 사용 여부를 바꿀 수 있습니다.

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
| `0x30` OTA_START | 서버 → ESP | 펌웨어 URL (ASCII, AGV ID 일치 시만) |
| `0x31` OTA_RESULT | ESP → 서버 | JSON: stage(start/progress/done/fail/busy), pct, err |
| `0x11` INFO_REQ | 서버 → ESP | 없음 — 장치 정보 요청 |
| `0x12` INFO | ESP → 서버 | JSON: agv, mac, ip, fw, part, ota, cfg, setup, up, dsp, dspb, dsp_build, dsp_st, staged |
| `0x32` DSP_OTA_START | 서버 → ESP | JSON: url, ver, crc32, size, job, apply — 메인보드 이미지(.dspfw) 스테이징 (AGV ID 일치 시만) |
| `0x33` DSP_OTA_RESULT | ESP → 서버 | JSON: job, stage(download/staged/start/enter/erase/progress/verify/done/fail), pct, ver, err |
| `0x34` DSP_OTA_APPLY | 서버 → ESP | JSON: job — 스테이징된 이미지를 메인보드에 적용 (AGV ID 일치 시만) |
| `0x40` CFG_CMD | 서버 → ESP | 설정 명령 한 줄 (텍스트, AGV ID 일치 시만) |
| `0x41` CFG_RESULT | ESP → 서버 | 명령 결과 텍스트 (seq = 요청 seq) |

- 하향 패킷은 설정된 `server` IP 에서 온 것만 받습니다. 서버 하향 패킷을 처음 받는 순간 새 펌웨어와 원격 변경 설정이 확정(롤백 취소)됩니다.
- STATUS JSON 에 `part`(실행 슬롯), `ota`(pending/valid/busy), `cfg`(saved/trial/default), `setup`(무선 설정 모드 1/0), `dsp`/`dspb`/`dsp_build`(메인보드 앱·부트로더 버전·빌드일), `dsp_st`(idle/downloading/staged/programming), `staged`(대기 중 이미지 {job, ver} 또는 null) 가 포함됩니다.
- 서버는 ESP 가 보낸 패킷의 송신 endpoint(IP:50000)를 기억했다가 그곳으로 하향 송신하면 됩니다.
- Wi-Fi 가 끊긴 동안의 상향 프레임은 **버퍼링하지 않고 폐기**합니다. 지난 교통제어 패킷이 늦게 도착하는 것보다 메인보드 재전송이 안전합니다. seq 는 계속 증가하므로 서버에서 누락 구간이 보입니다.

## 메인보드(TMS320F2808) 펌웨어 OTA

메인보드에 상주 부트로더(플래시 섹터 A)를 넣고, 브리지가 서버에서 받은 이미지를 UART 로 프로그래밍합니다. 설계·규격은 [docs/DSP_OTA_DESIGN.md](docs/DSP_OTA_DESIGN.md), 부트로더·앱 수정·CCS 설정은 [dsp/README.md](dsp/README.md).

```
dspota 7 stage app_1.2.3.dspfw        # (BridgeTest) 스테이징 — 주행 중에도 가능, 브리지가 dspfw 파티션에 보관
dspota 7 apply                        # 정지 상태에서 적용: 앱에 진입 요청 → 부트로더 → 지우기·쓰기·검증 → 재부팅 → 버전 확인
info 7                                # 메인보드 앱/부트로더 버전, 스테이징 상태
```

- 이미지는 CCS 빌드 결과를 `tools/dsp_pack.py` 로 만듭니다. 벤치에서 브리지 없이 PC 에서 직접 구울 때는 `tools/dsp_flash.py COM5 app.dspfw --enter 7`.
- 앱 헤더(CRC)를 마지막에 쓰므로 쓰는 중 전원이 끊겨도 부트로더가 대기하고, 다시 `apply` 하면 됩니다. 롤백 슬롯은 없고 이전 이미지를 다시 적용합니다.
- 적용 중(20~30초) 메인보드 앱과 RFID 바이패스가 멈춥니다. **AGV 정지 상태**에서만, 앱도 주행 중이면 거부(`app_ng_01`)합니다.
- 브리지 콘솔: `dsp` (상태) / `dsp ver` / `dsp apply`.
- **파티션 변경**: 0.5.0 부터 OTA 슬롯 1.75MB + `dspfw` 384KB. 이전 버전 브리지는 USB 로 한 번 전체를 다시 구워야 합니다.
  구 파티션 테이블인 채 OTA 로 0.5.x 를 받은 브리지는 `dsp_st` 를 `nopart` 로 보고하고 메인보드 OTA 명령을 `no_partition` 으로 거부합니다(0.5.1 부터. 0.5.0 은 이 경우 재부팅).
- 0.5.1: 스테이징 다운로드가 4KB 씩 지우며 받아 중계 지연이 줄고, HTTP 3xx 를 따라가며, 지우기 뒤 실패하면 메인보드 버전을 `?` 로 보고합니다. 부트로더·앱 지원 코드에 부트 카운터(기동 불능 앱 복구)와 파서 재동기가 추가되어 **앱은 `fwup_boot_ok()` 호출과 RAMM1 길이 0x3FA 가 필요**합니다 (`dsp/README.md`).

## 벤치 테스트 순서

1. **RS232 루프백**: BOB 의 T1OUT–R1IN 을 점퍼로 연결합니다. `mon 1` 상태에서 서버가 보낸 MAIN_DOWN 이 그대로 MAIN_UP 으로 돌아오는지 확인합니다.
2. **메인보드 대역 PC**: USB-RS232 어댑터를 채널 1 에 연결하고, 터미널(RealTerm 등, 115200)로 `STX…ETX` 프레임을 송수신합니다.
3. **RFID 리더 연결**: 태그를 읽혀 `RFID>` 덤프와 서버 수신을 확인합니다. 보드레이트가 안 맞으면 깨진 데이터로 보이니 `set rfid_baud` 로 맞춥니다.
4. **지연 측정**: `tools/BridgeTest.cs` 에서 `ping 7 100` 을 실행합니다. 교통제어 0.5초 목표 대비 여유를 확인하세요.

```bash
cd tools
dotnet run BridgeTest.cs     # .NET 10, 명령: list | send <agv> <data> | ping <agv> [n] | ota <agv> <bin|url> | cfg <agv> [cmd] | hb | quit
```

## 적용 전 확인·다음 단계

- AGV 장착 시에는 BOB-11189 대신 절연형(TD301D232H ×2)을 사용합니다. GND 전위차·모터 노이즈 대책입니다.
- 다중 AP 구간에서는 `menuconfig > Component config > Wi-Fi > Advanced support for Wi-Fi Roaming` 을 검토하세요(RSSI 기반 선제 로밍).
- Wi-Fi 재접속: 연속 5회까지는 즉시, 그 이후(AP 꺼짐·비밀번호 오류)는 1초 간격으로 시도합니다.
- 400대 운용 시 서버 측 Gateway(ACS 소켓 연동, OTA 순차 배포)가 필요합니다.
