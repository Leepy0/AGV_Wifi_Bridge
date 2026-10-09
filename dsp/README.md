# 메인보드(TMS320F2808) OTA — 부트로더·앱 지원 코드

설계: [docs/DSP_OTA_DESIGN.md](../docs/DSP_OTA_DESIGN.md). 이 디렉터리는 CCS 로 빌드하는 C28x 코드다.

```
dsp/common/   dsp_image.[ch]   이미지 헤더·CRC (부트로더·브리지·패커 테스트 공용)
              bootproto.[ch]   브리지 ↔ 부트로더 프로토콜 (장치 쪽 처리기 + 패킷 함수)
dsp/boot/     부트로더 (플래시 섹터 A)
dsp/app/      메인보드 앱에 추가하는 파일 + 링커 cmd 예시
tools/dsp_pack.py   CCS 빌드 결과 → OTA 이미지(.dspfw)
```

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
6. 클럭·보드레이트는 `boot_cfg.h` (20MHz 크리스탈, PLL ×10/2, 115200).

부트로더 크기: 약 2.4K 워드 + Flash API (16K 워드 섹터에 충분).

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
   `my_safe_check()` 는 0(안전) 또는 사유(1 주행, 2 충전, 3 전압, 4 에러, 9 기타)를 돌려준다.
   초기화가 끝나 메인 루프에 들어가면 **`fwup_boot_ok()` 를 한 번** 호출한다. 부트로더는 앱으로 점프할 때마다 부트 카운터를 올리고
   이 호출이 0 으로 되돌리는데, 호출 없이 워치독 리셋이 3회 이어지면 "기동 불능 앱"으로 보고 업데이트 모드에서 새 이미지를 기다린다.
4. 빌드 후 post-build step (CCS: Properties → Build → Steps → Post-build):
   ```
   "${CG_TOOL_HEX}" -i -o "${ProjName}.hex" "${BuildArtifactFileName}" --memwidth=16 --romwidth=16
   python tools/dsp_pack.py pack "${ProjName}.hex" -o "${ProjName}_1.2.3.dspfw" --ver 1.2.3
   ```
   버전 문자열은 **8자 이하**로 (`R` 응답의 버전 칸이 8자 고정이라 브리지가 앞 8자로 완료 여부를 판단).
   HEX 는 반드시 `--memwidth=16 --romwidth=16`(워드 주소) 로 뽑는다 — 바이트 주소 HEX 는 패커가 거부한다.
5. 첫 설치(1회, 유선): 부트로더(섹터 A) + 재링크한 앱(섹터 B~D) 을 JTAG 으로 굽고, 앱 헤더가 비어 있으므로
   **처음 한 번은 브리지(또는 BridgeTest 의 시리얼 모드)로 .dspfw 를 적용**해서 헤더를 채운다.
   헤더가 없으면 부트로더는 앱을 무효로 보고 업데이트 모드에서 기다린다.

## 3. 벤치 검증 순서

1. 부트로더만 굽고 리셋 → SCI-A 115200 으로 HELLO 응답 확인 (`BridgeTest dspserial COM5 hello`).
2. `BridgeTest dspserial COM5 app.dspfw` 로 앱 프로그래밍 → 리셋 후 앱 기동 확인.
3. 앱에서 `F3107V000` 프레임에 `R3107V…` 응답, `F3107U000` 에 `R3107UOK00` 후 부트로더 진입 확인.
4. 브리지 연결 → 서버(BridgeTest) `dspota 7 stage app.dspfw` → `dspota 7 apply` → `done` 보고와 heartbeat `dsp` 버전 확인.
5. 실패 주입: 쓰는 중 전원 차단 → 재부팅 시 부트로더 대기 → 다시 apply 로 복구. 잘못된 파일·CRC 오류 → 거부.
6. 기동 불능 앱: `fwup_boot_ok()` 를 부르지 않는(또는 바로 워치독 리셋되는) 앱을 올림 → 리셋 3회 뒤 HELLO 에 응답(업데이트 모드) → 정상 앱 apply 로 복구.
7. 브리지 재부팅 주입: WRITE 도중 브리지 `reboot` → 다음 apply 의 HELLO 가 첫 시도에 응답하는지 (파서 재동기).
