#pragma once
#include <stdint.h>

#define HW_SCI_NONE         (-1)        // hw_sci_getc: 받은 바이트 없음
#define HW_SCI_RXERR        (-2)        // hw_sci_getc: 수신 오류(브레이크 등) — SCI·RX FIFO 를 리셋했으니 파서도 재동기할 것

// 플래시에서 실행
void     hw_init_clocks(void);          // 워치독 정지, PLL, LSPCLK, SCI-A 클럭
void     hw_copy_ramfuncs(void);        // ramfuncs + Flash API → RAML0
void     hw_sci_init(void);
void     hw_sci_deinit(void);           // 앱 점프 전: SCI-A·핀을 리셋 상태로 되돌림 (앱이 리셋 기본값을 가정해도 되게)
int      hw_sci_getc(void);             // 수신 바이트(0..255), 없으면 HW_SCI_NONE, 수신 오류면 HW_SCI_RXERR
void     hw_sci_putc(uint16_t b);
void     hw_sci_flush(void);            // 송신 완료 대기
void     hw_timer_start(void);          // CPU 타이머 0 을 0µs 부터 다시 (1µs 단위, 약 71분마다 랩)
uint32_t hw_timer_us(void);             // hw_timer_start 뒤 경과 µs (랩이 지나면 0xFFFFFFFF 로 고정)
void     hw_timer_deinit(void);         // 앱 점프 전: 타이머 0 을 리셋 상태(분주 없음, 실행)로 되돌림
void     hw_reset(void);                // 즉시 리셋 (워치독)
void     hw_jump_app(void);

// RAM 에서 실행 (ramfuncs)
void     hw_init_flash(void);           // 대기 상태·파이프라인
uint16_t hw_flash_erase(uint16_t bp_mask);                             // BP_ERASE_* → BP_ST_*
uint16_t hw_flash_program(uint32_t addr, const uint16_t *w, uint16_t n); // 쓰고 검증 → BP_ST_*
