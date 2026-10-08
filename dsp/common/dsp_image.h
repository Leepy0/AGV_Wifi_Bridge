#pragma once
// 메인보드(F2808) 앱 이미지 형식 — 부트로더(C28x)·브리지(ESP32)·패커 테스트(호스트) 공용
// C28x 는 char 가 16비트이므로 "바이트"도 uint16_t(0..255) 로 다룬다. uint8_t 는 쓰지 않는다.
#include <stdint.h>

// 플래시 워드 주소
#define DSPIMG_APP_HDR_ADDR     0x3E8000UL   // 앱 헤더 64 워드 (섹터 D 시작)
#define DSPIMG_APP_CODE_ADDR    0x3E8040UL   // 앱 codestart (부트로더가 점프)
#define DSPIMG_APP_END          0x3F4000UL   // 앱 영역 끝 (exclusive) = 섹터 A 시작
#define DSPIMG_APP_MAX_WORDS    (DSPIMG_APP_END - DSPIMG_APP_CODE_ADDR)   // 0xBFC0 = 49088 워드
#define DSPIMG_BOOTINFO_ADDR    0x3F7F00UL   // 부트로더 정보 블록 64 워드

#define DSPIMG_HDR_WORDS        64
#define DSPIMG_MAGIC0           0xA55A
#define DSPIMG_MAGIC1           0x4650       // 'FP'
#define DSPIMG_HDR_VER          1
#define DSPIMG_VER_WORDS        32           // 버전 문자열: 워드당 1문자, NUL 종료 (최대 31자)

// 헤더 워드 위치
#define DSPIMG_W_MAGIC0         0
#define DSPIMG_W_MAGIC1         1
#define DSPIMG_W_HDRVER         2
#define DSPIMG_W_FLAGS          3
#define DSPIMG_W_LEN_LO         4            // app_len (워드 수, 0x3E8040 부터), 하위 워드 먼저
#define DSPIMG_W_LEN_HI         5
#define DSPIMG_W_CRC_LO         6            // app_crc32: [0x3E8040, +app_len) 의 CRC32
#define DSPIMG_W_CRC_HI         7
#define DSPIMG_W_BUILD_LO       8            // build_time: UTC unix 초
#define DSPIMG_W_BUILD_HI       9
#define DSPIMG_W_VER            10           // 10..41
#define DSPIMG_W_HDRCRC         62           // 워드 0..61 의 CRC16
#define DSPIMG_W_HDRCRC_N       63           // ~CRC16

// 부트로더 정보 블록 (0x3F7F00, 64 워드)
#define DSPIMG_BOOT_MAGIC0      0xB007
#define DSPIMG_BOOT_MAGIC1      0x1DEA
#define DSPIMG_BOOT_W_PROTO     2            // 프로토콜 버전
#define DSPIMG_BOOT_W_VER       4            // 4..19: 부트로더 버전 문자열 (16 워드, NUL 종료)

// 부트플래그 (RAM 0x0007FC, 4 워드): 앱이 쓰고 부트로더가 읽은 뒤 지움
#define DSPIMG_BOOTFLAG_ADDR    0x0007FCUL
#define DSPIMG_BOOTFLAG_0       0xB007
#define DSPIMG_BOOTFLAG_1       0x1DEA
#define DSPIMG_BOOTFLAG_2       0x5AFE
#define DSPIMG_BOOTFLAG_3       0xC0DE

typedef struct {
    uint32_t app_len;               // 워드
    uint32_t app_crc;
    uint32_t build_time;
    uint16_t flags;
    uint16_t ver[DSPIMG_VER_WORDS]; // 문자값 0..255, NUL 종료
} dspimg_hdr_t;

// 헤더 검사 결과
#define DSPIMG_OK               0
#define DSPIMG_E_MAGIC          1
#define DSPIMG_E_HDRCRC         2
#define DSPIMG_E_RANGE          3   // app_len 이 영역을 벗어남 / 0
#define DSPIMG_E_APPCRC         4

// CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, 최종 ~). 워드마다 하위 바이트 → 상위 바이트 순
uint32_t dspimg_crc32_init(void);
uint32_t dspimg_crc32_words(uint32_t crc, const uint16_t *w, uint32_t n);
uint32_t dspimg_crc32_bytes(uint32_t crc, const uint16_t *b, uint32_t n);   // b[i] = 0..255
uint32_t dspimg_crc32_final(uint32_t crc);

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), 같은 바이트 순서
uint16_t dspimg_crc16_words(const uint16_t *w, uint32_t n);
uint16_t dspimg_crc16_bytes(const uint16_t *b, uint32_t n);

// 64 워드 헤더 → 구조체. magic·hdr_crc·범위 검사 (앱 CRC 는 호출 측이 확인)
int  dspimg_hdr_parse(const uint16_t *hdr, dspimg_hdr_t *out);
// 구조체 → 64 워드 헤더 (hdr_crc 포함)
void dspimg_hdr_build(const dspimg_hdr_t *in, uint16_t *hdr);
