#pragma once
#include <stddef.h>

// 텍스트 설정 명령 — 서버 원격(UDP) / 휴대폰 BLE 공용. 키와 문법은 USB 콘솔과 같음
//   cfg [json] | set <key> <value> | save | discard | reboot | stat [json] | setup [on|off] | help  (+ 현장: defaults)
typedef enum {
    CMD_SRC_SERVER,     // 서버 원격: save = 확인 대기(재부팅 후 서버 통신 없으면 복구), defaults 불가
    CMD_SRC_LOCAL,      // 현장 무선(BLE): save = 즉시 확정, defaults 가능
} cmd_src_t;

typedef enum {
    CMD_ACT_NONE,
    CMD_ACT_REBOOT,     // 결과를 보낸 뒤 호출 측이 재부팅
} cmd_act_t;

// 응답 끝 표시 줄 (BR-8). 응답을 보낸 뒤 BLE·UDP 에 붙인다 — 앱은 이 줄을 보면 다음 명령을 보낸다
#define CMD_END_LINE ">\n"

// line 은 토큰 분리로 수정됨. 결과 텍스트를 out 에 기록하고 길이 반환 (항상 NUL 종료)
// 'cfg json' · 'stat json' 은 한 줄 JSON 을 반환 (BR-1). 'discard' 는 편집 사본을 실행 중 설정으로 되돌림
size_t cmd_exec(char *line, cmd_src_t src, char *out, size_t cap, cmd_act_t *act);
// 'cfg json' 본문 (콘솔 cfg json 과 공용)
size_t cmd_cfg_json(char *out, size_t cap);
