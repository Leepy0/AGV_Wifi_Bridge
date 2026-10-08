#pragma once
// 브리지 ↔ 메인보드 앱 프레임 (AGV-ACS 프레임 규격: STX + 종류 PAN MSG AGV(2) + 본문 + ' ' + BCC + ETX)
//  F (브리지 → AGV): V000 버전 조회 / U000 업데이트 진입
//  R (AGV → 브리지): V + 앱버전8 + 부트버전8 + 빌드일8 / U + OK00|NGxx
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DAP_STX 0x02
#define DAP_ETX 0x03
#define DAP_FRAME_MAX 48

typedef struct {
    char    cmd;            // 'V' / 'U'
    char    app_ver[9];     // V: 공백 제거, NUL 종료
    char    boot_ver[9];
    char    build[9];       // YYYYMMDD
    bool    ok;             // U: OK 여부
    uint8_t reason;         // U: NG 사유
} dap_reply_t;

char    dap_bcc(const uint8_t *payload, size_t len);
// F 프레임 생성 (STX..ETX). 반환 = 길이, cap 부족 시 0
size_t  dap_build_query(uint8_t *out, size_t cap, char pan, char msg, uint8_t agv, char cmd);
// 메인보드에서 온 프레임(STX..ETX)이 이 호기의 R 응답이면 해석. 0 = 성공, -1 = R 프레임 아님, -2 = 형식/BCC 오류
int     dap_parse_reply(const uint8_t *frame, size_t len, uint8_t agv, char expect_msg, dap_reply_t *r);
// 지나가는 메인보드 프레임에서 이 호기의 PAN ID 학습 (E 이벤트 등). 해당 없으면 0
char    dap_learn_pan(const uint8_t *frame, size_t len, uint8_t agv);
