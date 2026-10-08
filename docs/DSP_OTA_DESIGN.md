# 메인보드(TMS320F2808) OTA 설계 — B안: 상주 부트로더

> 상태: 설계 확정 단계. 구현 전 아래 "확인 필요" 항목을 채운 뒤 착수.
> 범위: DSP 부트로더 · 메인보드 앱 수정 · 브리지(ESP32) · 패커 · 펌웨어 관리 서버(향후).

## 1. 전체 구조

```
┌──────────────────┐  HTTP(이미지)      ┌──────────┐  UART 115200   ┌─────────────────┐
│ 펌웨어 관리 서버  │ ───────────────▶ │ 브리지    │ ─────────────▶ │ 메인보드 F2808   │
│ (Gateway 와 통합) │  UDP(명령/결과)   │ ESP32-S3 │  J4 (SCI-A)    │ 부트로더 + 앱     │
│ 이미지·대상·이력  │ ◀─────────────── │ 이미지 보관│ ◀───────────── │                 │
└──────────────────┘  heartbeat(버전)   └──────────┘                └─────────────────┘
```

- 서버는 **이미지 저장·대상 선정·실행 명령·진행 기록·버전 이력**을 맡는다. 장치는 상태를 보고만 한다 (서버가 기록의 원본).
- 브리지는 이미지를 미리 받아 자기 플래시에 보관(**스테이징**)하고, 서버의 적용 명령을 받으면 메인보드를 부트로더로 보내 로컬 UART 로 프로그래밍한다. Wi-Fi 상태는 프로그래밍 시간에 영향을 주지 않는다.
- 부트로더는 플래시 섹터 A 에 상주하며 절대 지우지 않는다. 앱 영역(섹터 B~D)만 교체한다.

### 업데이트 흐름

```
① 서버: 이미지 업로드 → 작업 생성(대상 호기 선택)
② 서버 → 브리지: DSP_OTA_START {url, ver, crc, size, job, apply}
③ 브리지: HTTP 수신 → dspfw 파티션에 저장 → CRC 확인 → "staged" 보고   (주행 중에도 가능)
④ 서버 → 브리지: DSP_OTA_APPLY {job}        (apply=1 이면 ③ 직후 자동)
⑤ 브리지 → 앱: `F…U000` 프레임 → 앱이 정지 상태 확인 후 `R…UOK`, 부트플래그 기록, 워치독 리셋
⑥ 부트로더: 플래그 확인 → 업데이트 모드 → 브리지와 HELLO
⑦ 브리지: ERASE(B~D) → WRITE(블록) → 헤더 마지막 → VERIFY → RESET
⑧ 앱 기동 → 브리지: `F…V000` 으로 버전 확인 → "done {ver}" 보고 → 서버가 이력 기록
실패: 어느 단계든 "fail {stage, err}" 보고. 이미지는 스테이징 상태로 남아 재시도 가능
```

## 2. 메인보드 메모리 배치

### 플래시 (64K 워드, 섹터 4개)

| 영역 | 주소 (워드) | 용도 |
|---|---|---|
| 섹터 D | 0x3E8000 – 0x3EBFFF | 앱 (앱 헤더 + 코드) |
| 섹터 C | 0x3EC000 – 0x3EFFFF | 앱 |
| 섹터 B | 0x3F0000 – 0x3F3FFF | 앱 |
| 섹터 A | 0x3F4000 – 0x3F7F7F | **부트로더** (OTA 로 교체 불가) |
| 예약 | 0x3F7F80 – 0x3F7FF5 | CSM 미사용 → 0xFFFF 유지 |
| BEGIN | 0x3F7FF6 – 0x3F7FF7 | `LB _c_int00` (부트로더 진입) |
| CSM 비밀번호 | 0x3F7FF8 – 0x3F7FFF | **0xFFFF 유지. 0x0000 쓰면 영구 잠김** |

앱 영역 = 0x3E8000 – 0x3F3FFF, **48K 워드(96KB)**. 현재 앱이 이 안에 들어가야 한다 (.map 의 FLASH 사용량 확인).

| 앱 영역 내부 | 주소 | 내용 |
|---|---|---|
| 앱 헤더 | 0x3E8000 – 0x3E803F (64 워드) | 아래 3절. **프로그래밍 때 맨 마지막에 씀** |
| 앱 codestart | 0x3E8040 | `LB _c_int00` — 부트로더가 여기로 점프 |
| 앱 코드·상수 | 0x3E8042 – 0x3F3FFF | 링커 cmd 로 배치 |

부트로더 정보 블록 (앱이 읽어 보고용): 0x3F7F00 – 0x3F7F3F (64 워드) — magic, 부트로더 버전 문자열, 프로토콜 버전.

### RAM (18K 워드)

| RAM | 주소 | 부트로더 | 앱 |
|---|---|---|---|
| M0 | 0x000000 – 0x0003FF | 스택 | 앱 자유 |
| M1 | 0x000400 – 0x0007FB | .ebss | 앱 자유 |
| **부트플래그** | **0x0007FC – 0x0007FF (4 워드)** | 읽고 지움 | 쓰기만. **양쪽 링커 cmd 에서 제외** |
| L0 | 0x008000 – 0x008FFF | 플래시 API + 쓰기 루틴 (ramfuncs) | 앱 자유 |
| L1 | 0x009000 – 0x009FFF | 수신 블록 버퍼 | 앱 자유 |
| H0 | 0x3F8000 – 0x3F9FFF | 미사용 | 앱 자유 |

부트로더와 앱은 동시에 돌지 않으므로 RAM 을 겹쳐 써도 된다. 부트플래그 4 워드만 둘 다 건드리지 않는다. SARAM 은 워치독 리셋을 거쳐도 내용이 남는다(전원 차단 시에만 소실).

## 3. 앱 이미지 형식

패커(`tools/dsp_pack.py`)가 CCS 빌드 결과에서 만든다. 서버에 올리는 파일이자 브리지가 보관하는 파일.

### 앱 헤더 (플래시 0x3E8000, 64 워드)

| 워드 | 내용 |
|---|---|
| 0 | magic0 = 0xA55A |
| 1 | magic1 = 0x4650 ('FP') |
| 2 | 헤더 형식 버전 = 1 |
| 3 | flags (예약, 0) |
| 4–5 | app_len: 코드 길이(워드), 0x3E8040 부터. 리틀 엔디언(하위 워드 먼저) |
| 6–7 | app_crc32: [0x3E8040, 0x3E8040 + app_len) 의 CRC32 |
| 8–9 | build_time: UTC unix 초 |
| 10–41 | version: 최대 31자 + NUL (C28x 는 char 가 16비트 → 워드당 1문자) |
| 42–61 | 예약 (0xFFFF) |
| 62 | hdr_crc16: 워드 0–61 의 CRC16 |
| 63 | ~hdr_crc16 |

CRC32: 표준(poly 0xEDB88320, init 0xFFFFFFFF, 최종 XOR). 워드마다 **하위 바이트, 상위 바이트 순**으로 넣는다. CRC16: CRC-16/CCITT-FALSE(poly 0x1021, init 0xFFFF), 같은 바이트 순서. 패커·부트로더·브리지가 모두 같은 구현을 쓴다(테스트 벡터를 문서에 고정).

**유효 조건**: magic 일치 ∧ hdr_crc 일치 ∧ app_crc32 일치. 쓰다가 끊기면 셋 중 하나가 반드시 깨진다.

### 이미지 파일 (`*.dspfw`)

```
[파일 헤더 32 B] magic "AGVDSP1\0", total_len, image_crc32, ver(16 B ASCII)
[앱 헤더 64 워드 = 128 B]
[블록 N개] 각 블록: addr(u32, 워드 주소) len(u16, 워드 수) data[len 워드, 리틀 엔디언 바이트쌍]
```
- 0xFFFF 로만 채워진 구간은 블록에서 뺀다 (전송량 절감).
- 블록 주소는 반드시 0x3E8040 – 0x3F3FFF 안. 범위 밖이면 패커가 거부. 부트로더도 거부.
- 서버는 파일 헤더의 ver·crc 를 그대로 DB 에 올린다.

### 패커 입력

`hex2000 -i -o app.hex app.out --memwidth=16 --romwidth=16` 의 Intel HEX. hex2000 의 주소 단위(워드/바이트)는 출력물로 자동 판별(플래시 영역이 0x3E8000 대면 워드, 0x7D0000 대면 바이트). 버전은 `--ver 1.2.3` 인자 또는 `.out` 안의 `g_app_version` 심볼에서 읽는다(둘 다 있으면 일치 확인).

## 4. 부트로더 (섹터 A)

### 리셋 후 동작

```
1. 워치독 정지, PLL·클럭 설정 (앱과 동일 값 — 크리스탈·PLLCR·DIVSEL 확인 필요)
2. ramfuncs(플래시 API + 쓰기 루틴) 를 L0 로 복사
3. 부트플래그(0x0007FC) 가 0xB007/0x1DEA/0x5AFE/0xC0DE 이면 → 지우고 업데이트 모드
4. 앱 헤더 검사 (magic, hdr_crc, app_crc32 — 48K 워드 CRC 는 수 ms)
   유효 → 0x3E8040 으로 점프 (앱이 자체 초기화. 부트로더는 인터럽트·주변장치를 건드리지 않음)
   무효 → 업데이트 모드
5. 업데이트 모드: SCI-A 115200 8N1, 프로토콜 대기. 가능하면 LED 점멸.
   10분간 수신 없고 앱이 유효하면 앱으로 리셋 (브리지가 죽은 경우 대비)
```

- 부팅 지연: 플래그 검사 + CRC ≈ 수 ms. 시리얼을 기다리지 않는다.
- 업데이트 모드에서 GPIO 는 리셋 기본값(입력) 그대로 둔다. 현재 전원 ON 직후와 같은 상태.
- 플래시 API: controlSUITE `Flash2808_API_V3_02.lib`. RAM 실행, 지우기·쓰기 중 인터럽트 금지, `Flash_CPUScaleFactor` 를 SYSCLK 에 맞춤, 워치독 콜백.
- **섹터 A 지우기 명령은 없다.** 주소 검사로 0x3F4000 이상 쓰기를 거부한다.

### 브리지 ↔ 부트로더 프로토콜 (SCI-A, 115200, 바이너리)

```
[0xC5][0x3A][len u16 LE][cmd u8][seq u8][payload len B][crc16 u16 LE]   crc16: cmd..payload
응답: cmd | 0x80, payload 첫 바이트 = status (0 OK, 그 외 오류코드)
```

| cmd | 이름 | payload | 응답 payload | 시간 |
|---|---|---|---|---|
| 0x01 | HELLO | — | status, boot_ver(16 B), proto_ver, app_valid, app_ver(32 B), max_block(워드) | 즉시 |
| 0x02 | ERASE | 섹터 마스크 (bit0=B, bit1=C, bit2=D) | status | 섹터당 1~2초 |
| 0x03 | WRITE | addr u32, nwords u16, data | status | 128 워드 ≈ 30ms |
| 0x04 | VERIFY | — | status, crc32 계산값, 헤더 유효 여부 | ~수 ms |
| 0x05 | RESET | 0 = 앱으로 | status 후 워치독 리셋 | — |
| 0x06 | READ | addr u32, nwords u16 | status, data | 진단용 |

- 블록 128 워드(256 B). 브리지 타임아웃: 일반 500ms, ERASE 15s. 재시도 3회(seq 로 중복 판별).
- WRITE 는 쓴 뒤 읽어 비교(Flash_Verify)까지 하고 응답한다.
- 쓰기 순서: 블록들 → **앱 헤더(0x3E8000) 마지막**.

## 5. 메인보드 앱에 추가할 것

### 링커·버전

| 항목 | 내용 |
|---|---|
| 링커 cmd | FLASH_APP: 0x3E8042 – 0x3F3FFF. `.apphdr`(64 워드) 를 0x3E8000, `codestart` 를 0x3E8040. 부트플래그 0x0007FC–0x0007FF 제외 |
| 버전 문자열 | 헤더는 패커가 채우므로 앱 소스에는 없어도 됨. 앱에서 자체 표시하려면 `g_app_version` 을 두고 패커가 일치 확인 |

### 브리지 ↔ 앱 프레임 (J4, 기존 AGV-ACS 프로토콜 형식)

기존 프레임 규격을 그대로 따른다: `STX` + 종류(1) + PAN ID(1) + MSG ID(1) + AGV ID(2) + 본문 + 공백 + BCC(1) + `ETX`. BCC 는 STX·ETX 를 뺀 바이트 합의 하위 니블을 ASCII-HEX 대문자로.

**새 종류 문자 `F`(브리지 → AGV) 와 `R`(AGV → 브리지) 는 현재 프로토콜 문서에 없는 값이다. 아래는 제안이며, 확정 후 프로토콜 문서에 등록한다.**

| 프레임 | 방향 | 본문 (헤더 뒤) | 예 (PAN 3, MSG 1, 07호기) |
|---|---|---|---|
| 버전 조회 | 브리지 → AGV | `V000` | `F3107V000 ` + BCC |
| 버전 응답 | AGV → 브리지 | `V` + 앱 버전 8자 + 부트로더 버전 8자 + 빌드일 8자(YYYYMMDD). 짧으면 공백 채움 | `R3107V1.2.3   1.0.0   20261008 ` + BCC |
| 업데이트 진입 | 브리지 → AGV | `U000` | `F3107U000 ` + BCC |
| 진입 응답 | AGV → 브리지 | `U` + `OK` 또는 `NG` + 사유 2자리 | `R3107UOK00 ` + BCC / `R3107UNG01 ` + BCC |

사유 코드(제안): `01` 주행 중, `02` 충전 중, `03` 전압 부족, `04` 에러 상태, `09` 기타.

- `F` 프레임은 브리지가 만든다. PAN ID 는 그 호기의 `E` 이벤트에서 학습한 값(모르면 `1`), MSG ID 는 브리지 자체 카운터 `1`~`F`.
- AGV 앱은 `F` 프레임을 ACS 의 ack·중복 판정 로직과 **분리**해서 처리한다 (MSG ID 가 ACS 패킷과 겹칠 수 있음). `F` 에 대한 `A` ack 는 보내지 않고 `R` 로 답한다.
- `R` 프레임은 브리지가 소비하고 서버로 올리지 않는다. `F`/`R` 외 프레임은 지금처럼 그대로 중계한다.
- 진입 응답 `UOK` 를 보낸 뒤 앱은 약 50ms 후 부트플래그를 기록하고 워치독 리셋한다. 안전 조건(정지, 충전 아님, 전압 정상, 에러 없음)은 메인보드 기준으로 정의한다.
- 브리지 쪽 프레임 생성·해석은 `main/dsp_app_proto.h/.c` 한 곳에 두고 BCC 는 레퍼런스의 테스트 벡터(`H`→`8`, `12345678`→`4`)로 검증한다.
- 체크섬이 4비트라 수신 측은 길이·종류·자리별 문자 범위도 같이 검사한다.

## 6. 브리지(ESP32) 변경

### 파티션 (USB 로 1회 재굽기 필요 — 현장 배포 전에 확정)

| 이름 | 오프셋 | 크기 | 비고 |
|---|---|---|---|
| ota_0 | 0x020000 | 0x1C0000 (1.75MB) | 1.9MB → 축소 (현재 앱 1.2MB) |
| ota_1 | 0x1E0000 | 0x1C0000 | |
| **dspfw** | 0x3A0000 | 0x60000 (384KB) | 스테이징 이미지 + 메타(job, ver, crc). 재부팅 후에도 유지 |

### UDP 패킷 추가

| type | 방향 | payload |
|---|---|---|
| `0x11` INFO_REQ | 서버 → ESP | 없음 |
| `0x12` INFO | ESP → 서버 | JSON: agv, mac, ip, fw(ESP), part, ota, cfg, dsp(앱 버전), dspb(부트로더 버전), dsp_build, staged{job, ver} |
| `0x32` DSP_OTA_START | 서버 → ESP | JSON: url, ver, crc32, size, job, apply(0/1) |
| `0x33` DSP_OTA_RESULT | ESP → 서버 | JSON: job, stage(staged/enter/erase/progress/verify/done/fail), pct, ver, err |
| `0x34` DSP_OTA_APPLY | 서버 → ESP | JSON: job |

- heartbeat(STATUS) 에 `dsp`, `dspb`, `staged` 필드 추가. 서버는 이것으로 전 호기 버전 현황을 갱신한다.
- 브리지는 부팅 후, 그리고 DSP 업데이트 후에 버전 조회(`F…V000`)를 보내 `dsp`/`dspb` 를 갱신한다. 응답 없으면 `"?"` (미지원 구 펌웨어).

### 프로그래밍 중 UART 독점

- `uart_bridge` 에 원시 모드 추가: ETX 프레이밍 해제, RFID→메인보드 바이패스 정지, 서버 MAIN_DOWN 폐기(카운트), 프로그래밍 태스크가 바이트 단위로 읽고 씀. 끝나면 복귀.
- 조건: ESP OTA 진행 중이면 거부. 스테이징된 이미지의 CRC 가 맞지 않으면 거부.
- 실패 시 재시도: 부트로더는 앱이 무효한 동안 업데이트 모드에 머무르므로 HELLO 부터 3회까지 자동 재시도. 그래도 실패하면 fail 보고. 이미지는 남겨 서버가 다시 APPLY 할 수 있게 한다.

## 7. 펌웨어 관리 서버 (향후)

Gateway(ACS 연동, UDP 50000) 와 같은 프로세스 또는 그 옆에 둔다. BridgeTest.cs 가 참조 구현이며, 실제 서버는 .NET 으로 만들어도 같은 패킷을 쓴다.

### 데이터

| 테이블 | 필드 |
|---|---|
| devices | agv_id(PK), mac, last_ip, esp_fw, esp_part, esp_ota_state, dsp_ver, dsp_boot_ver, dsp_build, cfg_state, rssi, last_seen, staged_job |
| images | id, kind(esp/dsp), ver, file_path, size, crc32/sha256, uploaded_at, uploaded_by, note |
| jobs | id, image_id, kind, mode(stage/apply), created_at, created_by, state(open/done/cancelled), note |
| job_items | job_id, agv_id, state(queued/staged/programming/done/failed), pct, started_at, finished_at, before_ver, after_ver, error |
| events | ts, agv_id, job_id, stage, detail (DSP_OTA_RESULT·OTA_RESULT 원문) |

### 기능

1. **이미지 관리**: 업로드(.dspfw / ESP .bin) → 헤더에서 ver·crc 추출 → HTTP 로 서비스 (`/fw/<id>`).
2. **대상 선정**: 호기 목록(버전·접속·상태) 에서 선택, 필터(현재 버전 ≠ 대상 버전, 접속 중, 정지 중).
3. **실행**: 스테이징(즉시, 병렬 가능) → 적용(정지 확인 후. ACS 상태 연동 또는 수동 "지금 적용"). 동시 적용 수 제한 옵션.
4. **진행·기록**: 결과 패킷을 job_items/events 에 기록. 완료 시 before/after 버전과 시각. heartbeat 의 `dsp` 가 대상 버전과 일치해야 완료로 인정.
5. **이력 조회**: 호기별 업데이트 이력, 버전별 호기 수, 실패 목록과 사유.
6. **롤백**: 이전 이미지를 골라 같은 절차로 재설치 (슬롯 롤백은 없음).
7. **ESP OTA 도 같은 작업 모델**로 다룬다 (kind=esp, 기존 OTA_START/OTA_RESULT).

시각은 서버 기준으로 기록한다. 장치에는 RTC 가 없다.

## 8. 안전·운용 규칙

- 적용은 **정지 상태**에서만. 서버(ACS)와 앱(`U000` 진입 조건) 이중 확인.
- 적용 중 메인보드는 앱을 돌리지 않는다(약 20~30초). 이 동안 RFID 바이패스도 멈춘다.
- 전원이 끊기면 앱 헤더가 깨진 채 남고, 다음 부팅에 부트로더가 대기한다. 서버에서 다시 APPLY.
- 부트로더는 OTA 로 못 고친다. 배포 전 벤치에서 충분히 검증(전원 차단·케이블 분리·잘못된 이미지·CRC 오류 주입).
- 첫 설치(부트로더 + 재링크 앱)는 호기마다 유선(JTAG 또는 SDFlash) 1회.

## 9. 확인 필요 (구현 착수 전)

| # | 항목 | 용도 |
|---|---|---|
| 1 | .map 의 FLASH 사용량 (워드) | 48K 워드 이하 확인. 여유가 적으면 최적화 옵션 검토 |
| 2 | 크리스탈 주파수, PLLCR, PLLSTS.DIVSEL, LSPCLK 설정 | 부트로더 클럭·보드레이트·Flash_CPUScaleFactor |
| 3 | SCI-A 핀(GPIO28/29 기본?), J4 보드레이트 | 부트로더 SCI 설정 |
| 4 | 새 종류 문자 `F`/`R` 와 본문 형식 승인, PAN ID 운용 방식 | 5절 프레임 확정 → 프로토콜 문서 등록 |
| 5 | CCS·컴파일러 버전, controlSUITE Flash API 보유 여부 | 부트로더 빌드 환경 |
| 6 | CSM 비밀번호 위치 0x3F7FF8–0x3F7FFF 가 0xFFFF | 안전 확인 |
| 7 | 표시용 LED GPIO (있으면) | 업데이트 모드 표시 |

## 10. 구현 순서

1. 패커 + 이미지 형식 + CRC 테스트 벡터 (호스트 테스트)
2. 부트로더 소스·링커 cmd·앱 수정 예시 → CCS 빌드 → JTAG 으로 벤치 검증 (PC 에서 BridgeTest 로 직접 시리얼 프로그래밍)
3. 브리지: 파티션, 패킷, 스테이징, UART 원시 모드, 프로그래밍 클라이언트, INFO/heartbeat
4. BridgeTest: `dspota stage/apply/info` 명령 (서버 참조 구현)
5. 관리 서버 (별도 과제)
