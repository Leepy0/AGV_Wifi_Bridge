#pragma once
// 메인보드 패키지(.dsppkg = .dspfw+.out+.map) 보관 — dsppkg 파티션의 슬롯 2개 (A/B, 0.9.0~).
// 브리지가 메인보드에 굽는 모든 프로그램은 짝 패키지가 여기 있어야 한다 (ZIP 은 풀지 않고 바이트 그대로 보관).
// 슬롯 n = n × 6MB: 머리 섹터 4KB(머리 128 B) + 패키지 바이트(오프셋 0x1000). 배치·상태: docs/DSP_OTA_DESIGN.md 6절
// flags 비트는 1→0 으로만 바꾼다 (지우기 없이 한 워드 쓰기): bit0=0 기록 완료, bit1=0 적용됨, bit2=0 지난 것
// dsp 태스크만 쓰고, 상태 조회(dsp_pkg_slot 등)는 다른 태스크에서 읽기만 한다
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DSP_PKG_LABEL       "dsppkg"
#define DSP_PKG_SLOTS       2
#define DSP_PKG_SLOT_SIZE   0x600000u
#define DSP_PKG_HDR_SECTOR  0x1000u
#define DSP_PKG_MAX         (DSP_PKG_SLOT_SIZE - DSP_PKG_HDR_SECTOR)    // 6,287,360 B

typedef enum { DSP_PKG_EMPTY, DSP_PKG_STAGED, DSP_PKG_APPLIED, DSP_PKG_OLD } dsp_pkg_state_t;

typedef struct {
    dsp_pkg_state_t st;     // 해석한 상태 (두 슬롯이 모두 APPLIED 면 seq 작은 쪽을 OLD 로 본다)
    uint32_t flags;         // flash 의 flags 원값
    uint32_t seq, size, job, dspfw_crc;
    char     id[17];        // 패키지 ID (소문자 hex 16자)
    char     ver[16];
    uint8_t  sha[32];
} dsp_pkg_slot_t;

bool        dsp_pkg_init(void);                     // 파티션 찾기·슬롯 머리 읽기. false = 파티션 없음 (nopart)
bool        dsp_pkg_ready(void);
const dsp_pkg_slot_t *dsp_pkg_slot(int i);          // 0~1
int         dsp_pkg_current(void);                  // APPLIED 슬롯, 없으면 -1
int         dsp_pkg_find_staged(uint32_t dspfw_crc);    // 짝 STAGED 슬롯 (여럿이면 seq 큰 쪽), 없으면 -1
const char *dsp_pkg_state_name(dsp_pkg_state_t st); // STAGED / APPLIED / OLD / empty

// 기록: begin → write … → end(크기·sha256) → finalize(짝 .dspfw 스테이징 뒤 bit0 → 0). 중간 실패는 abort (슬롯은 빈 채로)
// 오류 이름: pkg_nopart · pkg_too_big · pkg_size · pkg_sha · pkg_flash
const char *dsp_pkg_begin(uint32_t size, uint32_t job, const char *id, const uint8_t sha[32]);
const char *dsp_pkg_write(uint8_t *buf, size_t n);  // buf 는 flash 에서 다시 읽어 해시하는 데 쓰인다 (내용이 바뀜)
const char *dsp_pkg_end(void);
const char *dsp_pkg_finalize(uint32_t dspfw_crc, const char *ver);
void        dsp_pkg_abort(void);

const char *dsp_pkg_verify(int slot);               // flash 에서 sha256 을 다시 계산해 머리 값과 비교. NULL = 일치
const char *dsp_pkg_mark_applied(int slot);         // bit1 → 0, 다른 슬롯이 APPLIED 였으면 그 슬롯 bit2 → 0
// 'dsp pkg' 출력 (슬롯마다 한 줄). staged_crc = 스테이징된 .dspfw 파일 CRC (짝 표시, 0 = 없음)
size_t      dsp_pkg_format(char *buf, size_t cap, uint32_t staged_crc);
