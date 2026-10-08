#pragma once
// 메인보드 앱에 추가하는 펌웨어 업데이트 지원 (B안 부트로더용)
//  1) 링커 cmd 를 F2808_app_example.cmd 처럼 바꾼다 (앱 = 섹터 B~D, 헤더 0x3E8000, codestart 0x3E8040, 부트플래그 제외)
//  2) 이 파일과 app_fwupdate.c 를 프로젝트에 넣는다 (.apphdr 자리표시자 포함)
//  3) J4 수신 프레임 처리에서 fwup_handle() 을 먼저 호출한다. 'F' 프레임이면 응답을 만들어 주므로 송신한다
//  4) FWUP_REPLY_RESET 을 받으면 응답 송신이 끝난 뒤(TXEMPTY) fwup_reset() 을 호출한다
//  5) 빌드 후: hex2000 -i -o app.hex app.out --memwidth=16 --romwidth=16 → tools/dsp_pack.py pack
#include <stdint.h>

#define FWUP_NONE           0   // F 프레임 아님 (기존 처리 계속)
#define FWUP_REPLY          1   // reply 를 송신
#define FWUP_REPLY_RESET    2   // reply 송신 후 fwup_reset()

#define FWUP_REPLY_MAX      48  // reply 버퍼 최소 크기 (STX..ETX)

// 업데이트 진입 안전 조건: 0 = 안전, 1 주행 중, 2 충전 중, 3 전압 부족, 4 에러 상태, 9 기타
typedef uint16_t (*fwup_safe_fn)(void);

// payload = STX 와 ETX 사이 전체 (종류…본문 + 공백 + BCC). agv_id = 이 호기 번호(1~99)
int  fwup_handle(const char *payload, uint16_t len, uint16_t agv_id, fwup_safe_fn safe,
                 char *reply, uint16_t cap, uint16_t *reply_len);
void fwup_reset(void);                      // 부트플래그 기록 → 워치독 리셋 (돌아오지 않음)
const char *fwup_app_version(void);         // 앱 헤더의 버전 문자열 (패커가 기록)
const char *fwup_boot_version(void);        // 부트로더 정보 블록의 버전 (없으면 "-")
char fwup_bcc(const char *payload, uint16_t len);   // AGV-ACS 체크섬 (하위 니블 ASCII-HEX)
