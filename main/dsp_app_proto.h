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

// ───────── 메인보드 OTA 검사 도우미 (ESP 의존 없음 — 호스트에서 시험: tests/dsp_ota_host) ─────────

#define DAP_VER_MAX 8       // 메인보드 앱 버전 최대 길이 (R 응답 버전 칸 8자 = 서버 DspVersion 규칙)

// 서버 DspVersion 규칙: 1~8자, [0-9A-Za-z._-] 만
bool    dap_ver_valid(const char *v);

// JSON 문자열 값으로 안전하게 복사 (" \ 와 제어 문자 이스케이프, 앞뒤 따옴표 없음). 항상 NUL 종료, 반환 = 길이.
// cap 이 모자라면 이스케이프 단위로 자른다
size_t  dap_json_escape(char *dst, size_t cap, const char *src);

// 평면 JSON {"key":"str","key":123,...} 전용 최소 파서 (중첩·이스케이프 없음 — 서버가 그렇게 보냄)
#define DAP_JSON_OK         1
#define DAP_JSON_MISSING    0       // 키 없음 / 문자열 아님 / 닫는 따옴표 없음
#define DAP_JSON_TOO_LONG   (-1)    // 값이 cap - 1 자를 넘음 (잘라 쓰지 않음, dst 는 빈 문자열)
int      dap_json_str(const char *json, size_t len, const char *key, char *dst, size_t cap);
uint32_t dap_json_u32(const char *json, size_t len, const char *key);     // 없으면 0

// URL(http:// 또는 https://) 의 호스트가 IPv4 주소 ip[4] (a.b.c.d 순) 와 같은지. 사용자 정보(@)·IPv6·호스트명은 false
bool    dap_url_host_is(const char *url, const uint8_t ip[4]);

// .dspfw 블록 순회 검사 (dsp_pack.py unpack 과 같은 규칙) — 메인보드를 지우기 전에 이미지 결함을 잡는다.
//  rd(ctx, off, buf, n): 파일 오프셋 off 에서 n 바이트 읽기 (성공 true)
//  블록: 앱 헤더(파일 32 + 128 B) 뒤부터 [addr u32][nw u16][data nw 워드] 반복, 파일 끝과 정확히 맞아야 함.
//  nw 1~BP_BLOCK_WORDS(128), [addr, addr+nw) ⊂ [앱 코드 시작, 앱 영역 끝), 주소 오름차순·겹침 없음(패커 출력 순서),
//  빈 구간은 0xFFFF 로 채워 [앱 코드 시작, +app_len) 의 CRC32 를 계산해 app_crc 와 비교 (app_len 밖의 워드는 제외)
#define DAP_IMG_OK          0
#define DAP_IMG_E_READ      1
#define DAP_IMG_E_BLOCK     2       // 블록 길이·파일 끝 불일치
#define DAP_IMG_E_ADDR      3       // 앱 영역 밖 / 순서 어긋남
#define DAP_IMG_E_CRC       4       // 복원 CRC32 ≠ app_crc
typedef bool (*dap_read_fn)(void *ctx, uint32_t off, void *buf, size_t n);
int     dap_image_check_blocks(dap_read_fn rd, void *ctx, uint32_t file_len, uint32_t app_len, uint32_t app_crc);
