#pragma once
// ESP ↔ 서버(Gateway) UDP 패킷 규격 — 서버 측과 동일하게 유지할 것
// [magic 2][ver 1][type 1][agv_id 1][flags 1][seq 2][len 2][payload len]  (리틀 엔디언, 헤더 10바이트)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BR_MAGIC0       0xA5
#define BR_MAGIC1       0x5A
#define BR_VER          1
#define BR_HDR_LEN      10
#define BR_PAYLOAD_MAX  512

typedef enum {
    BR_T_MAIN_UP   = 0x01, // 메인보드 → 서버 (UART 원본 바이트)
    BR_T_MAIN_DOWN = 0x02, // 서버 → 메인보드 (UART 원본 바이트)
    BR_T_RFID      = 0x03, // RFID 리더 → 브로드캐스트/서버
    BR_T_STATUS    = 0x10, // 상태 heartbeat (JSON 텍스트)
    BR_T_INFO_REQ  = 0x11, // 서버 → ESP, 장치 정보 요청
    BR_T_INFO      = 0x12, // ESP → 서버, JSON: agv, mac, ip, fw, part, ota, cfg, dsp, dspb, staged ...
    BR_T_PING      = 0x20, // 서버 → ESP, 왕복 지연 측정
    BR_T_PONG      = 0x21, // ESP → 서버, PING 의 seq/payload 그대로 반송
    BR_T_OTA_START = 0x30, // 서버 → ESP, payload = 펌웨어 URL (ASCII, http://서버/파일.bin)
    BR_T_OTA_RESULT= 0x31, // ESP → 서버, JSON: stage(start/progress/done/fail/busy), pct, err
    BR_T_DSP_OTA_START = 0x32, // 서버 → ESP, JSON: url, ver, crc32, size, job, apply — 메인보드 이미지 스테이징
    BR_T_DSP_OTA_RESULT= 0x33, // ESP → 서버, JSON: job, stage(download/staged/start/enter/erase/progress/verify/done/fail), pct, ver, err
    BR_T_DSP_OTA_APPLY = 0x34, // 서버 → ESP, JSON: job — 스테이징된 이미지를 메인보드에 적용
    BR_T_CFG_CMD   = 0x40, // 서버 → ESP, payload = 설정 명령 한 줄 (ASCII, 예: "set ip 192.168.10.107")
    BR_T_CFG_RESULT= 0x41, // ESP → 서버, 명령 결과 텍스트 (seq = 요청 seq)
} br_type_t;

typedef struct {
    uint8_t  type;
    uint8_t  agv_id;
    uint8_t  flags;
    uint16_t seq;
    uint16_t len;
} br_hdr_t;

// 헤더+payload 를 out 에 기록, 기록한 전체 길이 반환 (공간 부족·payload 초과 시 0)
size_t br_build(uint8_t *out, size_t cap, uint8_t type, uint8_t agv_id,
                uint16_t seq, const uint8_t *payload, size_t len);

// 수신 패킷 검증·파싱. 성공 시 true, payload 는 in 내부를 가리킴
bool br_parse(const uint8_t *in, size_t n, br_hdr_t *hdr, const uint8_t **payload);
