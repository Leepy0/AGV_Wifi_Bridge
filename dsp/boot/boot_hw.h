#pragma once
#include <stdint.h>

// 플래시에서 실행
void     hw_init_clocks(void);          // 워치독 정지, PLL, LSPCLK, SCI-A 클럭
void     hw_copy_ramfuncs(void);        // ramfuncs + Flash API → RAML0
void     hw_sci_init(void);
int      hw_sci_getc(void);             // 수신 바이트(0..255), 없으면 -1
void     hw_sci_putc(uint16_t b);
void     hw_sci_flush(void);            // 송신 완료 대기
void     hw_timer_start(void);          // 32비트 자유 실행 (SYSCLK), 약 43초마다 랩
int      hw_timer_wrapped(void);        // 랩 플래그 읽고 클리어
uint32_t hw_timer_now(void);            // 현재 카운터 값 (하강). 경과 = 이전값 - 현재값
void     hw_reset(void);                // 즉시 리셋 (워치독)
void     hw_jump_app(void);

// RAM 에서 실행 (ramfuncs)
void     hw_init_flash(void);           // 대기 상태·파이프라인
uint16_t hw_flash_erase(uint16_t bp_mask);                             // BP_ERASE_* → BP_ST_*
uint16_t hw_flash_program(uint32_t addr, const uint16_t *w, uint16_t n); // 쓰고 검증 → BP_ST_*
