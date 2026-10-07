# 실기 업로드 가이드 (ESP32-S3 AGV Wi-Fi Bridge)

처음 한 번은 USB 로 전체 이미지를 굽고, 호기 설정을 한 뒤, 이후 버전은 OTA(무선)로 올립니다.

```
① 빌드 zip 받기 → ② USB 연결·포트 확인 → ③ 전체 이미지 굽기 → ④ 부팅 확인
→ ⑤ 호기 설정(휴대폰 / 콘솔 / 서버) → ⑥ 동작 확인 → ⑦ 이후 업데이트는 OTA
```

## 1. 준비물

- 빌드 zip `agv_wifi_bridge_<버전>.zip`
  - GitHub **Releases**(`v*` 태그) 또는 **Actions** 실행 결과의 Artifacts 에서 받습니다.
- ESP32-S3 DevKitC-1 (플래시 4MB 이상), **데이터 전송이 되는** USB 케이블 (충전 전용 케이블은 포트가 안 보입니다)
- PC 에 esptool
  - Windows: Arduino IDE 에 ESP32 보드가 설치돼 있으면 `flash.bat` 이 내장 `esptool.exe` 를 자동으로 찾습니다.
  - 그 외: `pip install esptool`

zip 내용:

| 파일 | 용도 | 주소 |
|---|---|---|
| `agv_wifi_bridge_merged.bin` | **처음 굽기** — 부트로더+파티션+앱 전체 | 0x0 |
| `agv_wifi_bridge.bin` | **OTA 업데이트용** 앱만 | (ota_0 = 0x20000) |
| `bootloader.bin` | 개별 굽기용 | 0x0 |
| `partition-table.bin` | 개별 굽기용 | 0x8000 |
| `ota_data_initial.bin` | 부팅 슬롯 초기화 | 0xf000 |
| `flash_args` | 개별 파일 주소 목록 (esptool v5: `esptool --chip esp32s3 -p COM5 write_flash @flash_args`) | |
| `flash.bat` / `flash.sh` | 굽기 스크립트 (Windows / Linux·macOS) | |

## 2. USB 연결·포트 확인

DevKitC-1 에는 USB 포트가 두 개 있습니다. **"UART" 라고 적힌 쪽**에 연결합니다. 굽기와 `bridge>` 콘솔 입력이 모두 이 포트입니다. ("USB" 쪽 포트에도 로그는 나오지만 명령 입력은 안 됩니다.)

| OS | 포트 이름 | 확인 방법 |
|---|---|---|
| Windows | `COM5` 등 | 장치 관리자 → 포트(COM & LPT) → *Silicon Labs CP210x* 또는 *USB-Enhanced-SERIAL CH343* |
| Linux | `/dev/ttyUSB0` (CP210x) / `/dev/ttyACM0` (CH343) | `ls /dev/ttyUSB* /dev/ttyACM*` — 권한 오류 시 `sudo usermod -aG dialout $USER` 후 재로그인 |
| macOS | `/dev/cu.usbserial-*` / `/dev/cu.wchusbserial*` | `ls /dev/cu.*` |

포트가 안 보이면 USB-UART 드라이버(CP210x VCP 또는 CH343)를 설치하고 케이블을 바꿔 봅니다.

## 3. 처음 굽기 (전체 이미지)

zip 을 푼 폴더에서 실행합니다. 시리얼 모니터(Arduino IDE, PuTTY 등)는 **먼저 닫습니다**.

```bat
:: Windows
flash.bat COM5
```

```bash
# Linux / macOS
chmod +x flash.sh
./flash.sh /dev/ttyUSB0
```

스크립트 없이 직접 실행:

```bash
esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0x0 agv_wifi_bridge_merged.bin
```

`Hash of data verified.` 후 자동으로 재부팅되면 완료입니다.

> **주의 — 전체 이미지는 저장된 설정을 지웁니다.**
> merged.bin 은 NVS(설정 저장 영역, 0x9000)까지 덮어씁니다. 이미 설정한 호기를 USB 로 다시 구우면 호기 설정이 초기화되고, 부팅 시 설정 AP 가 자동으로 열립니다(처음 설치 상태).
> 설정을 유지하려면 아래처럼 **앱과 부팅 슬롯만** 굽습니다.
>
> ```bash
> esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0xf000 ota_data_initial.bin 0x20000 agv_wifi_bridge.bin
> ```
>
> 완전히 지우고 새로 시작하려면 `esptool --chip esp32s3 -p COM5 erase_flash` 후 전체 이미지를 굽습니다.
>
> 개별 파일로 굽는 경우 — 부트로더·파티션까지 새로 쓰지만 NVS(0x9000) 는 건드리지 않으므로 **설정이 유지됩니다**:
>
> ```bash
> esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 agv_wifi_bridge.bin
> ```

### 연결이 안 될 때 (`Failed to connect to ESP32-S3`)

1. 시리얼 모니터가 포트를 잡고 있지 않은지 확인합니다.
2. 보드의 **BOOT 버튼을 누른 채 RESET 을 눌렀다 떼고**, BOOT 를 뗀 뒤 다시 실행합니다 (다운로드 모드 수동 진입).
3. 케이블이 길거나 불안정하면 속도를 낮춥니다: `-b 460800` 또는 `-b 115200`.

## 4. 부팅 확인 (시리얼 콘솔)

터미널을 **115200 bps** 로 엽니다 (PuTTY, Tera Term, Arduino IDE 시리얼 모니터, 또는 `python -m serial.tools.miniterm COM5 115200`). RESET 을 누르면 다음과 같은 로그가 보입니다.

```
I (...) cfg: no saved config, using defaults
I (...) net: connecting to "AGV_TEST" ...
W (...) setup: setup AP "AGVBR-01-A1B2" open - http://192.168.4.1
bridge>
```

- 처음 굽은 보드는 저장된 설정이 없어서 **설정 AP 가 자동으로 열립니다.**
- Enter 를 치면 `bridge>` 프롬프트가 나오고, `help` 로 명령 목록을 볼 수 있습니다.

## 5. 호기 설정

아래 셋 중 편한 방법을 씁니다. 모두 **저장 후 재부팅**해야 적용됩니다.

### A. 휴대폰 (현장 권장)

1. 휴대폰 Wi-Fi 에서 `AGVBR-<호기>-<MAC 끝 4자리>` 에 접속합니다 (비밀번호 기본 `agvsetup`).
   - "인터넷 연결 없음" 경고가 나오면 **연결 유지**를 선택합니다. 페이지가 안 열리면 모바일 데이터를 잠시 끕니다.
2. 브라우저에서 `http://192.168.4.1` 을 엽니다.
3. AGV ID, SSID, 비밀번호, 고정 IP·마스크·게이트웨이, 서버 IP 를 입력하고 **저장 후 재부팅**을 누릅니다.
4. 재부팅하면 설정 AP 는 닫히고 공장 Wi-Fi 에 접속합니다.

AP 가 닫힌 뒤 다시 열려면: **BOOT 버튼 3초** / 콘솔 `setup on` / 서버 `cfg <agv> setup on`.

### B. USB 콘솔

```
bridge> set agv 7
bridge> set ssid AGV_WIFI
bridge> set pass mypassword
bridge> set ip 192.168.10.107
bridge> set mask 255.255.255.0
bridge> set gw 192.168.10.1
bridge> set server 192.168.10.10
bridge> set rfid_baud 9600
bridge> cfg          # 확인
bridge> save
bridge> reboot
```

### C. 서버 원격 (이미 네트워크에 붙은 호기)

```
cfg 7 set ip 192.168.10.107
cfg 7 save
cfg 7 reboot
```

원격 저장은 재부팅 후 180초 안에 서버 패킷이 없으면 이전 설정으로 자동 복구됩니다. 서버(Gateway)가 주기적으로 PING 을 보내는지 확인하세요.

## 6. 동작 확인

| 확인 | 방법 | 정상 |
|---|---|---|
| Wi-Fi | 콘솔 `stat` | `wifi: UP ip=192.168.10.107 rssi=-40~-70` |
| 서버 수신 | BridgeTest `list` | `[07] 192.168.10.107:50000 last=0.x s ago` |
| 지연 | BridgeTest `ping 7 100` | 손실 0%, max 가 교통제어 목표(0.5초)보다 충분히 작음 |
| 상태 보고 | BridgeTest `hb` | `"cfg":"saved"`, `"ap":0`, `"ota":"valid"` |
| 프레임 | 콘솔 `mon 1` + RS232 루프백 | MAIN_DOWN 이 MAIN_UP 으로 되돌아옴 (README 벤치 테스트 참고) |

배선은 README 의 **기본 핀** 표를 따릅니다 (3V3 / GND / GPIO17·18 메인보드, GPIO4·5 RFID). AGV 장착 시에는 BOB-11189 대신 절연형(TD301D232H ×2)을 사용합니다.

## 7. 이후 업데이트 (OTA, 무선)

USB 없이 서버에서 올립니다. **AGV 정지 상태에서, 여러 대는 한 대씩** 진행합니다.

```
ota 7 agv_wifi_bridge.bin      # BridgeTest — PC 가 HTTP(8070)로 파일을 제공하고 URL 전달
```

1. `OTA {"stage":"start"...}` → `progress`(10% 단위) → `done` 후 재부팅
2. 새 펌웨어가 서버 패킷을 받으면 확정됩니다 (`"ota":"valid"`).
3. 180초 안에 서버 통신이 없으면 이전 펌웨어로 자동 롤백합니다.

OTA 는 앱만 바꾸므로 **호기 설정은 유지됩니다.** PC 방화벽에서 TCP 8070 / UDP 50000·50001 을 허용해야 합니다.

## 8. 문제 해결

| 증상 | 확인 / 조치 |
|---|---|
| 포트가 안 보임 | 데이터 케이블인지, USB-UART 드라이버(CP210x / CH343) 설치 |
| `Failed to connect` | 시리얼 모니터 닫기 → BOOT 누른 채 RESET → 속도 낮추기 |
| 콘솔에 아무것도 안 나옴 | "UART" 포트인지, 115200 인지, RESET 눌러 보기 |
| 계속 재부팅 (`rst:0x...` 반복) | USB 전원 부족(허브 대신 PC 직결), 플래시 4MB 미만 보드 |
| `wifi: DOWN` 유지 | `stat` 의 `last_reason`: **201** AP 없음(SSID·2.4GHz·채널 1~13 확인), **15 / 204** 비밀번호 오류, **202** 인증 실패, **210·211** 보안 방식 불일치(WPA2 지원 확인) |
| 설정 AP 가 안 보임 | 저장된 설정이 있으면 자동으로 안 열림 → BOOT 3초 또는 `setup on` |
| 192.168.4.1 이 안 열림 | 휴대폰 모바일 데이터 끄기, "연결 유지", 주소 직접 입력 (`http://` 포함) |
| 서버에 아무것도 안 옴 | 서버 IP·포트, PC 방화벽 UDP 50000/50001, `stat` 의 `tx_err` |
| 서버 하향이 안 감 | 서버가 설정된 `server` IP 로 보내는지 (`rx_foreign` 증가 = 다른 IP·다른 호기) |
| 원격 설정 3분 뒤 원래대로 | 재부팅 후 서버가 하향 패킷(PING)을 안 보냄 → Gateway 주기 PING 확인 |
| RFID 가 깨져 보임 | `set rfid_baud` 를 리더 보드레이트에 맞춤 |
