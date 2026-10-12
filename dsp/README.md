# 메인보드(TMS320F2808) OTA — 부트로더·앱 지원 코드

설계: [docs/DSP_OTA_DESIGN.md](../docs/DSP_OTA_DESIGN.md). 이 디렉터리는 CCS 로 빌드하는 C28x 코드다. 기준은 브리지 **v0.5.2** 소스다.

```
dsp/common/   dsp_image.[ch]   이미지 헤더·CRC (부트로더·브리지·패커 테스트 공용)
              bootproto.[ch]   브리지 ↔ 부트로더 프로토콜 (장치 쪽 처리기 + 패킷 함수)
dsp/boot/     부트로더 (플래시 섹터 A)
dsp/app/      메인보드 앱에 추가하는 파일 + 링커 cmd 예시
tools/dsp_pack.py   CCS 빌드 결과 → OTA 이미지(.dspfw)
tests/host/   호스트 시험 (gcc·python3) — CI host-test 와 같음
```

> **부트로더는 OTA 로 바꿀 수 없다.** 현장 설치 전에 이 소스(0.5.2)로 빌드한 부트로더를 JTAG 으로 굽는다.
> 0.5.1 이전 소스로 구운 부트로더도 0.5.2 브리지와 통신은 되지만, 아래 HELLO 수신 창(전원 재투입 복구)·부트플래그 60초 복귀·SCI 수신 오류 복구가 없다.
> 0.5.2 소스의 부트로더 버전 문자열은 `1.1.0` 이다(이전은 `1.0.0`). 게이트웨이 호기 상세의 '부트로더'(`dspb`)가 `1.0.0` 인 호기는 아직 옛 부트로더이므로 JTAG 으로 다시 굽는다.

## 1. 부트로더 프로젝트 (CCS 9.1)

1. 새 프로젝트: TMS320F2808, 출력 COFF, **Large memory model(-ml)**, unified memory(-mt). 컴파일러 18.12.x LTS.
2. 소스 추가: `dsp/boot/*.c`, `dsp/boot/codestart.asm`, `dsp/common/dsp_image.c`, `dsp/common/bootproto.c`.
   include 경로: `dsp/boot`, `dsp/common`, Flash API 의 include.
3. **Flash API (TI SPRC193, F280x Flash API V3.02)**: `Flash2808_API_V302.lib` 와 `Flash2808_API_Library.h`, `Flash2808_API_Config.h` 를 프로젝트에 넣는다.
   - `Flash2808_API_Config.h` 의 `CPU_RATE` 가 10.000L(100MHz) 인지 확인.
   - 라이브러리 파일명이 다르면 `F2808_boot.cmd` 의 `Flash2808_API_V302.lib` 를 맞춘다.
   - 헤더의 함수 원형·`FLASH_ST`·`SECTORx` 가 `boot_hw.c` 에서 쓰는 것과 같은지 확인.
4. 링커 cmd: `F2808_boot.cmd` 만 쓴다 (DSP280x_Headers cmd 불필요 — 레지스터는 `f2808_regs.h` 직접 정의).
   `f2808_regs.h` 의 주소·비트는 DSP280x_SysCtrl.h / DSP280x_Sci.h / DSP280x_Gpio.h 와 한 번 대조한다.
5. 빌드 → JTAG 으로 굽기. **지울 섹터는 A 만** 선택(CCS 플래시 설정). 앱이 있으면 B~D 는 그대로 둔다.
6. 클럭·보드레이트·플래시 대기 상태는 `boot_cfg.h` (20MHz 크리스탈, PLL ×10/2 = 100MHz, 115200, RANDWAIT 3 · PAGEWAIT 3 · OTPWAIT 5).

부트로더 크기: 약 2.4K 워드 + Flash API (16K 워드 섹터에 충분).

### 부트로더가 하는 일

| 상황 | 동작 |
|---|---|
| 앱 유효, 정상 부팅 | 부트 카운터 +1 → **200ms 동안 HELLO 만 받는 창** → HELLO 가 없으면 SCI·타이머를 리셋 기본값으로 되돌리고 앱(0x3E8040)으로 점프. **매 부팅이 약 0.2초 늦어진다** |
| HELLO 창에서 HELLO 를 받음 | 업데이트 모드 (브리지 복구 적용 중 전원 재투입). 창 길이는 고정이라 노이즈가 와도 늘지 않고, HELLO 외의 명령은 응답 없이 버린다 |
| 부트플래그 (앱이 `F…U000` 을 승인) | 업데이트 모드. **첫 유효 패킷까지 60초**, 그 안에 오지 않고 앱이 유효하면 앱으로 리셋. 첫 패킷 뒤에는 600초 유휴 기준 |
| 부트 카운터 3 초과 (`fwup_boot_ok()` 없이 연속 리셋) | 업데이트 모드. 유효한 패킷 없이 600초가 지나면 앱으로 리셋 |
| 앱 무효 (헤더·CRC 깨짐, 첫 설치) | 업데이트 모드에서 무기한 대기 |

- 패킷 수신 도중 100ms 넘게 끊기면 파서를 처음부터 다시 맞춘다(HELLO 창 안에서는 10ms).
- SCI 수신 오류(브레이크·프레이밍·오버런·패리티)가 나면 보내던 응답을 끝낸 뒤 SCI 와 RX FIFO 를 리셋하고 다음 패킷부터 다시 받는다. 쓰는 도중 브리지가 재부팅해도 메인보드 전원을 다시 넣지 않고 HELLO 부터 이어 갈 수 있다.
- 재전송 판정: seq·명령·요청 길이·요청 CRC 가 모두 직전과 같을 때만 다시 처리하지 않고 이전 응답을 돌려준다.
- READ(진단)는 플래시 0x3E8000~0x3F7FF7 과 OTP 0x3D7800~0x3D7BFF 만 읽는다. 섹터 A 끝의 CSM 비밀번호(0x3F7FF8~0x3F7FFF)와 주변장치 레지스터는 `BAD_ADDR`.
- `tools/dsp_flash.py` 는 실행마다 seq 를 무작위 값에서 시작한다(연속 실행한 두 도구가 이전 응답을 받지 않게).

## 2. 메인보드 앱 수정

1. 링커 cmd 를 `dsp/app/F2808_app_example.cmd` 처럼 바꾼다: 플래시는 `APPHDR`(0x3E8000) · `APPBEGIN`(0x3E8040) · `FLASHAPP`(0x3E8042~0x3F3FFF) 만, RAMM1 은 길이 0x3FA 로 (0x7FA~0x7FF 는 부트 카운터·부트플래그).
   기존 cmd 의 `codestart : > BEGIN` 을 `codestart : > APPBEGIN` 으로.
2. `dsp/app/app_fwupdate.c/.h` 와 `dsp/common/dsp_image.h` 를 추가한다 (`.apphdr` 자리표시자 포함).
3. J4 수신 프레임 처리에 끼워 넣는다:
   ```c
   char reply[FWUP_REPLY_MAX]; uint16_t rlen;
   int r = fwup_handle(payload, len, MY_AGV_ID, my_safe_check, reply, sizeof(reply), &rlen);
   if (r != FWUP_NONE) {
       sci_send(reply, rlen);                 // R 응답 송신
       if (r == FWUP_REPLY_RESET) {
           wait_tx_empty();                   // SCICTL2.TXEMPTY
           fwup_reset();                      // 부트플래그 → 워치독 리셋 → 부트로더 업데이트 모드
       }
       return;
   }
   ```
   - **`my_safe_check` 는 반드시 넘긴다.** 0(안전) 또는 사유(1 주행, 2 충전, 3 전압, 4 에러, 9 기타)를 돌려준다. NULL 을 넘기면 `U` 요청을 항상 사유 9 로 거부한다(`R…UNG09`, 게이트웨이에는 `app_ng_09`). 정지 여부를 아는 곳은 앱뿐이다.
   - 요청은 정확히 11자 형식만 받는다: `F` + PAN + MSG + 호기 2자리 + `V` 또는 `U` + `000` + 공백 + BCC (PAN·MSG 는 `1`~`9`·`A`~`F`). 어긋나면 `FWUP_NONE` 이고 안전 콜백도 부르지 않는다.
4. 초기화가 끝나 메인 루프에 들어가면 **`fwup_boot_ok()` 를 한 번** 호출한다. 부트로더는 앱으로 점프할 때마다 부트 카운터를 올리고
   이 호출이 0 으로 되돌리는데, 호출 없이 리셋이 3회 이어지면 "기동 불능 앱"으로 보고 업데이트 모드에서 새 이미지를 기다린다.
5. **앱 워치독을 반드시 켠다.** codestart(WD_DISABLE)·InitSysCtrl 이 워치독을 끄므로 초기화 뒤 다시 켜고(SCSR WDENINT = 0 리셋 모드, WDCR = 0x0028 등) 메인 루프에서 주기적으로 서비스(ServiceDog)한다.
   - 앱이 멈추면 워치독 리셋 → 부트 카운터가 올라 3회 뒤 업데이트 모드 → OTA 로 다시 고칠 수 있다.
   - 워치독 없이 멈추는 앱은 리셋이 나지 않아 이 경로로 못 살린다. 그때는 게이트웨이 복구 적용 중 `wait_power`(메인보드 전원을 껐다 켜세요) 안내가 뜨면 60초 안에 메인보드 전원을 다시 넣어 HELLO 창으로 들어가게 해야 한다.
6. 빌드 후 post-build step (CCS: Properties → Build → Steps → Post-build):
   ```
   "${CG_TOOL_HEX}" -i -o "${ProjName}.hex" "${BuildArtifactFileName}" --memwidth=16 --romwidth=16
   python tools/dsp_pack.py pack "${ProjName}.hex" -o "${ProjName}_1.2.3.dspfw" --ver 1.2.3
   ```
   - 버전 문자열은 **영문·숫자·`.` `_` `-` 로 1~8자가 필수**다 (예: `1.2.3`, `2.0.15`). `R` 응답의 버전 칸이 8자 고정이라서다. 어기면 `dsp_pack.py` 가 패킹을 거부한다.
   - HEX 는 반드시 `--memwidth=16 --romwidth=16`(워드 주소) 로 뽑는다 — 바이트 주소 HEX 는 패커가 거부한다.
7. 첫 설치(1회, 유선): 부트로더(섹터 A) + 재링크한 앱(섹터 B~D) 을 JTAG 으로 굽고, 앱 헤더가 비어 있으므로
   **처음 한 번은 브리지(또는 PC 에서 `tools/dsp_flash.py`)로 .dspfw 를 적용**해서 헤더를 채운다.
   헤더가 없으면 부트로더는 앱을 무효로 보고 업데이트 모드에서 기다린다. 게이트웨이에서는 앱 버전이 `?` 이고 앱 프레임도 없으므로, 일반 적용으로도 브리지가 HELLO 로 부트로더에 바로 들어간다('복구 적용' 은 필요 없다).

## 3. 벤치 검증 순서

1. 부트로더만 굽고 리셋 → SCI-A 115200 으로 HELLO 응답 확인 (`python tools/dsp_flash.py COM5 --hello` → `boot_ver  : 1.1.0`, `app_valid : False` 등 항목별 한 줄).
2. `python tools/dsp_flash.py COM5 app.dspfw` 로 앱 프로그래밍 → 리셋 후 앱 기동 확인.
3. 앱에서 `F3107V000` 프레임에 `R3107V…` 응답, `F3107U000` 에 `R3107UOK00` 후 부트로더 진입 확인 (`dsp_flash.py COM5 --query 7`, `dsp_flash.py COM5 app.dspfw --enter 7`).
   safe 콜백 없이 빌드한 앱은 `R3107UNG09` 로 거부해야 한다.
   `fwup_handle()` 이 없는 구 앱이 프레임을 보내는 중이면(최근 3초 안에 STX 프레임) 브리지 적용은 원시 모드에 들어가지 않고 `app_no_fwup` 으로 끝나야 한다(중계 유지, `recover` 여도 같음).
   앱이 프레임을 3초보다 드물게 보내면 브리지는 앱이 조용하다고 본다. 이때 버전을 알면 `app_no_reply` 로 끝나고 `dsp` 가 `?` 로 바뀌어야 한다.
4. 브리지 연결 → 서버(BridgeTest) `dspota 7 stage app.dspfw apply`(스테이징 후 바로 적용) → `done` 보고와 heartbeat `dsp` 버전 확인.
   스테이징만 한 뒤(`apply` 없이) `dspota 7 apply` 를 보내면 그 호기에 마지막으로 stage 한 job 번호로 적용한다. BridgeTest 를 다시 띄웠으면 `dspota 7 apply <job>` 으로 `info 7`·heartbeat 의 `staged.job` 을 준다(다르면 `job_mismatch`).
   같은 job 의 APPLY 는 10분 동안 마지막 결과만 다시 보고하므로, 실패 뒤 다시 적용할 때는 `stage … apply` 를 다시 보내거나(새 job 번호라 다시 받고 굽는다) `cfg 7 dsp apply`(브리지 콘솔은 `dsp apply`, job 검사 없음)를 쓴다.
5. 실패 주입: 쓰는 중 전원 차단 → 재부팅 시 부트로더 대기 → 다시 적용으로 복구. 잘못된 파일·CRC 오류·블록 구조 오류 → 메인보드를 지우기 전에 거부(`image_crc`·`image_bad`·`ver_invalid`).
6. 기동 불능 앱: `fwup_boot_ok()` 를 부르지 않는(또는 바로 워치독 리셋되는) 앱을 올림 → 리셋 3회 뒤 HELLO 에 응답(업데이트 모드) → 정상 앱 적용으로 복구.
7. 브리지 재부팅 주입: WRITE 도중 브리지 `reboot` → 다음 적용의 HELLO 가 첫 시도에 응답하는지 (파서 재동기, SCI 수신 오류 복구). 메인보드 전원을 다시 넣지 않아도 이어져야 한다.
   0.5.2 브리지는 굽는 중 `reboot` 을 거부하므로 이 시험은 브리지 전원이나 RESET 버튼으로 끊는다.
8. 멈춘 앱 복구: 워치독을 끄고 무한 대기하는 앱을 올림 → 일반 적용은 `app_no_reply`(버전을 알던 경우) 또는 `no_bootloader`(버전 `?`)로 끝나야 한다 → 게이트웨이에서 **복구 적용** 을 켠 새 작업(BridgeTest 는 `dspota 7 stage app.dspfw` 로 새로 스테이징한 뒤 `dspota 7 apply recover`) → `wait_power` 가 뜨면 메인보드 전원을 껐다 켬 → 굽기가 이어져 `done`.
9. 부트플래그 60초: `--enter 7` 로 부트로더에 넣은 뒤 패킷을 보내지 않음 → 약 60초 뒤 앱으로 돌아오는지.
10. 부팅 지연: 정상 앱의 리셋 → 앱 시작까지 약 0.2초 늘어난 것을 확인하고, 앱의 기동 시간 조건과 충돌하지 않는지 본다.

호스트 시험(실기 없이): `bash tests/host/run.sh` — 프로토콜·재동기·재전송 판정·READ 범위, `fwup_handle` 판정과 브리지 프레임 계약, 가상 하드웨어 위의 `boot_main.c`(HELLO 창 200ms·부트플래그 60초/600초·앱 무효 무기한 대기·부트 카운터·수신 오류 재동기), `dsp_pack.py` selftest(블록 역순·겹침 거부 포함)와 기준 .dspfw 재현.
