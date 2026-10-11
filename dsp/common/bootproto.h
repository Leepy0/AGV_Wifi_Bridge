#pragma once
// 브리지 ↔ 부트로더 시리얼 프로토콜 (SCI-A 115200 8N1, 바이너리)
//  패킷: [0xC5][0x3A][len u16 LE][cmd u8][seq u8][payload len B][crc16 u16 LE]  crc16: cmd..payload
//  응답: cmd | 0x80, payload 첫 바이트 = status
// 이 파일은 장치(부트로더) 쪽 처리기와 양쪽 공용 패킷 함수를 담는다. 호스트에서도 컴파일된다 (uint8_t 사용 안 함).
#include <stdint.h>
#include "dsp_image.h"

#define BP_SOF0             0xC5
#define BP_SOF1             0x3A
#define BP_PROTO_VER        1
#define BP_BLOCK_WORDS      128                         // WRITE 최대 워드 수
#define BP_MAX_PAYLOAD      (6 + BP_BLOCK_WORDS * 2)    // WRITE: addr4 + n2 + data
#define BP_MAX_PACKET       (8 + BP_MAX_PAYLOAD)
#define BP_HELLO_RESP_LEN   61
#define BP_BOOTVER_LEN      16

#define BP_CMD_HELLO        0x01
#define BP_CMD_ERASE        0x02    // payload: mask (bit0 = B, bit1 = C, bit2 = D)
#define BP_CMD_WRITE        0x03    // payload: addr u32 LE (워드), n u16 LE, data n 워드 (하위 바이트 먼저)
#define BP_CMD_VERIFY       0x04
#define BP_CMD_RESET        0x05    // payload: mode (0 = 앱으로 재부팅)
#define BP_CMD_READ         0x06    // payload: addr u32, n u16 → data (진단)
#define BP_RESP             0x80

#define BP_ST_OK            0
#define BP_ST_BAD_ADDR      1
#define BP_ST_BAD_LEN       2
#define BP_ST_ERASE_FAIL    3
#define BP_ST_PROG_FAIL     4
#define BP_ST_VERIFY_FAIL   5
#define BP_ST_BAD_CMD       6
#define BP_ST_BAD_PAYLOAD   7

#define BP_ERASE_B          0x01
#define BP_ERASE_C          0x02
#define BP_ERASE_D          0x04
#define BP_ERASE_APP        0x07

// READ(진단) 허용 범위 (워드 주소, 끝은 exclusive) — 주변장치 레지스터(예: SCIRXBUF)를 읽어 부수효과가 생기지 않게
#define BP_READ_FLASH_START 0x3E8000UL      // 플래시 전체: 앱(섹터 D~B) + 부트로더(섹터 A)
#define BP_READ_FLASH_END   0x3F8000UL
#define BP_READ_OTP_START   0x3D7800UL      // OTP 1K 워드
#define BP_READ_OTP_END     0x3D7C00UL

// 장치 쪽 하드웨어 콜백
typedef struct {
    uint16_t (*erase)(uint16_t mask);                                   // BP_ERASE_* 마스크 → BP_ST_*
    uint16_t (*program)(uint32_t addr, const uint16_t *w, uint16_t n);  // 쓰고 읽어 비교까지 → BP_ST_*
    void     (*read)(uint32_t addr, uint16_t *w, uint16_t n);           // 플래시 읽기 (워드 주소)
    const uint16_t *boot_ver;                                           // 16 "바이트", NUL 종료
} bp_ops_t;

typedef struct {
    const bp_ops_t *ops;
    uint16_t state, len, idx;
    uint16_t rx[BP_MAX_PACKET];
    uint16_t tx[BP_MAX_PACKET];
    uint16_t tx_len;
    uint16_t reset_pending;         // RESET 응답을 보낸 뒤 호출 측이 리셋 수행 (mode 는 reset_mode)
    uint16_t reset_mode;
    uint16_t last_seq, last_cmd;    // 같은 요청(seq·cmd·길이·CRC 모두 같음)의 재전송이면 다시 처리하지 않고 응답만 반복
    uint16_t last_len, last_crc;
    uint16_t last_tx[BP_MAX_PACKET];
    uint16_t last_tx_len;
    uint16_t accept_cmd;            // 0 = 모든 명령. 0 아니면 이 명령만 처리하고 나머지는 무응답 (부팅 직후 HELLO 창)
} bp_ctx_t;

void bp_init(bp_ctx_t *c, const bp_ops_t *ops);
// 수신 바이트 하나 처리. 응답이 준비되면 1 (c->tx, c->tx_len) — 호출 측이 송신
int  bp_rx_byte(bp_ctx_t *c, uint16_t b);
// 패킷 수신 도중인지 (SOF 뒤 본문 대기 중). 호출 측이 바이트 간 공백이 길면 bp_rx_reset() 으로 재동기한다
int  bp_rx_busy(const bp_ctx_t *c);
void bp_rx_reset(bp_ctx_t *c);

// 양쪽 공용
uint16_t bp_crc16(const uint16_t *b, uint16_t n);
// 패킷 조립: out 에 전체 패킷, 반환 = 길이 (out 은 BP_MAX_PACKET 이상)
uint16_t bp_build(uint16_t *out, uint16_t cmd, uint16_t seq, const uint16_t *payload, uint16_t n);
// 앱 헤더·CRC 검사 (ops->read 로 플래시 읽기). 반환 DSPIMG_*; hdr/crc_out 은 NULL 가능
int  bp_app_check(const bp_ops_t *ops, dspimg_hdr_t *hdr, uint32_t *crc_out);
