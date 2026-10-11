#pragma once
// 메인보드 앱에 추가하는 펌웨어 업데이트 지원 (B안 부트로더용)
//  1) 링커 cmd 를 F2808_app_example.cmd 처럼 바꾼다 (앱 = 섹터 B~D, 헤더 0x3E8000, codestart 0x3E8040, 부트플래그 제외)
//  2) 이 파일과 app_fwupdate.c 를 프로젝트에 넣는다 (.apphdr 자리표시자 포함)
//  3) J4 수신 프레임 처리에서 fwup_handle() 을 먼저 호출한다. 'F' 프레임이면 응답을 만들어 주므로 송신한다
//  4) FWUP_REPLY_RESET 을 받으면 응답 송신이 끝난 뒤(TXEMPTY) fwup_reset() 을 호출한다
//  5) 초기화가 끝나 메인 루프에 들어가면 fwup_boot_ok() 를 한 번 호출한다 (부트 카운터 0).
//     안 부르면 부트로더가 "기동 불능 앱"으로 보고 워치독 리셋 3회 뒤 업데이트 모드에 머문다
//  6) [필수] 앱 워치독을 켠다. codestart(WD_DISABLE)·InitSysCtrl 이 워치독을 끄므로, 초기화 뒤 다시 켜고
//     (SCSR WDENINT = 0: 리셋 모드, WDCR = 0x0028 등) 메인 루프에서 주기적으로 서비스(ServiceDog)한다.
//     앱이 멈추면 워치독 리셋 → 부트 카운터가 올라가 3회 뒤 부트로더 업데이트 모드 → OTA 로 다시 고칠 수 있다.
//     워치독 없이 멈추는 앱은 리셋이 나지 않아 이 경로로 못 살린다. 그때는 브리지 구조 적용(HELLO 연속 송신) 중에
//     메인보드 전원을 다시 넣어 부트로더의 부팅 직후 HELLO 창(약 200ms)으로 들어가게 해야 한다.
//  7) 빌드 후: hex2000 -i -o app.hex app.out --memwidth=16 --romwidth=16 → tools/dsp_pack.py pack
//     (--ver 는 영문·숫자·. _ - 로 1~8자. R 응답의 버전 칸이 8자 고정이라 더 길면 서버 버전 비교가 실패한다)
#include <stdint.h>

#define FWUP_NONE           0   // F 프레임 아님 (기존 처리 계속)
#define FWUP_REPLY          1   // reply 를 송신
#define FWUP_REPLY_RESET    2   // reply 송신 후 fwup_reset()

#define FWUP_REPLY_MAX      48  // reply 버퍼 최소 크기 (STX..ETX)

// 업데이트 진입 안전 조건: 0 = 안전, 1 주행 중, 2 충전 중, 3 전압 부족, 4 에러 상태, 9 기타
// 반드시 넘길 것: NULL 이면 'U' 요청을 항상 거부한다 (UNG09). 정지 여부를 아는 곳은 앱뿐이다.
typedef uint16_t (*fwup_safe_fn)(void);

// payload = STX 와 ETX 사이 전체 (종류…본문 + 공백 + BCC). agv_id = 이 호기 번호(1~99)
// 요청 형식이 정확히 F PAN MSG AGV(2) V|U "000" 공백 BCC (11자, PAN·MSG 는 1~9·A~F) 가 아니면 FWUP_NONE
int  fwup_handle(const char *payload, uint16_t len, uint16_t agv_id, fwup_safe_fn safe,
                 char *reply, uint16_t cap, uint16_t *reply_len);
void fwup_reset(void);                      // 부트플래그 기록 → 워치독 리셋 (돌아오지 않음)
void fwup_boot_ok(void);                    // 정상 기동 알림: 부트 카운터 0 (메인 루프 진입 시 한 번. F 프레임 처리 때도 자동)
const char *fwup_app_version(void);         // 앱 헤더의 버전 문자열 (패커가 기록)
const char *fwup_boot_version(void);        // 부트로더 정보 블록의 버전 (없으면 "-")
char fwup_bcc(const char *payload, uint16_t len);   // AGV-ACS 체크섬 (하위 니블 ASCII-HEX)
