#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// NVS 에 blob 으로 저장되는 런타임 설정 (구조 변경 시 CFG_VERSION 증가)
#define CFG_VERSION 1

typedef struct {
    uint16_t version;
    uint8_t  agv_id;
    char     ssid[33];
    char     pass[65];
    uint32_t static_ip;     // 0 = DHCP (IP 값은 모두 네트워크 바이트 순서)
    uint32_t netmask;
    uint32_t gateway;
    uint32_t server_ip;
    uint16_t server_port;
    uint16_t local_port;
    uint32_t rfid_dst_ip;   // 255.255.255.255 = 브로드캐스트, 0 = 네트워크 전송 안 함
    uint16_t rfid_dst_port;
    uint32_t main_baud;
    uint32_t rfid_baud;
    uint16_t hb_ms;         // 0 = heartbeat 끔
} app_cfg_t;

extern app_cfg_t g_cfg;             // 실행 중 설정 (부팅 후 읽기 전용)
extern volatile bool g_monitor;     // 콘솔 'mon 1' → 프레임 hex 덤프

void      cfg_load(void);           // NVS → g_cfg (없거나 버전 불일치 시 Kconfig 기본값)
esp_err_t cfg_save(void);           // 편집 사본 → NVS
esp_err_t cfg_erase(void);
void      cfg_print(void);          // 편집 사본 표시
bool      cfg_set(const char *key, const char *val);   // 편집 사본만 변경, 재부팅 후 반영
