#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define FRAME_MAX 256

typedef enum { PORT_MAIN = 0, PORT_RFID = 1, PORT_COUNT } port_id_t;

// 수신 프레임 콜백 (UART 수신 태스크 컨텍스트에서 호출)
typedef void (*frame_cb_t)(const uint8_t *data, size_t len);

typedef struct {
    uint32_t rx_frames, rx_bytes;
    uint32_t tx_frames, tx_bytes;
    uint32_t overflow;      // FIFO/링버퍼 오버플로
    uint32_t rx_err;        // 프레이밍/패리티 오류
    uint32_t overlong;      // FRAME_MAX 초과로 잘린 프레임
    uint32_t resync;        // 이벤트 유실 후 프레임 경계 재동기 횟수
    uint32_t no_etx;        // ETX 모드에서 ETX 없이 타임아웃으로 끊긴 프레임
    uint32_t tx_drop;       // 메인보드 송신 폐기 (큐 가득 참 / 길이 초과)
} uart_stat_t;

esp_err_t uart_bridge_start(frame_cb_t on_main, frame_cb_t on_rfid);

// 메인보드로 보낼 프레임을 큐에 넣음. 프레임 단위로 송신되어 RFID/ACS 데이터가 섞이지 않음
bool uart_bridge_send_main(const uint8_t *data, size_t len);

const uart_stat_t *uart_bridge_stat(port_id_t port);
