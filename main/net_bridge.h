#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t tx_pkts, tx_err;
    uint32_t tx_offline;    // Wi-Fi 미연결로 폐기 (재전송은 메인보드 담당)
    uint32_t rx_pkts, rx_bad;
    uint32_t rx_foreign;    // 서버 외 송신원 / 다른 호기 대상
    uint32_t disconnects;
    uint8_t  last_reason;   // 마지막 연결 끊김 사유 (wifi_err_reason_t)
} net_stat_t;

esp_err_t net_bridge_start(void);
bool      net_is_up(void);
void      net_sta_hold(bool hold);   // true = STA 재접속 보류 (설정 AP 에 단말 접속 중 채널 유지용)

bool net_send_main(const uint8_t *data, size_t len);   // 메인보드 프레임 → 서버
bool net_send_rfid(const uint8_t *data, size_t len);   // RFID 프레임 → rfid_dst (브로드캐스트/서버)
bool net_send_ota_result(const uint8_t *data, size_t len);   // OTA 진행/결과 JSON → 서버

void   net_print_status(void);
size_t net_format_status(char *buf, size_t cap);   // stat 2줄 (wifi / net)
