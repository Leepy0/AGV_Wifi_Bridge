#pragma once
#include <stdint.h>
#include <stddef.h>
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

// 편집 사본은 콘솔·서버 원격 명령·설정 AP 웹 페이지가 함께 사용 (내부 잠금)
void      cfg_load(void);           // NVS → g_cfg (없거나 버전 불일치 시 Kconfig 기본값)
// 편집 사본 → NVS. trial = 서버 원격 저장: 재부팅 후 서버 통신이 없으면 이전 설정으로 자동 복구
esp_err_t cfg_save(bool trial);
esp_err_t cfg_erase(void);
void      cfg_print(void);          // 편집 사본 표시
size_t    cfg_format(char *buf, size_t cap);   // cfg_print 와 같은 내용을 buf 에 기록
void      cfg_get_edit(app_cfg_t *out);        // 편집 사본 복사
bool      cfg_set(const char *key, const char *val);   // 편집 사본만 변경, 재부팅 후 반영
// 여러 키를 한 번에 변경 — 하나라도 잘못되면 아무것도 바꾸지 않고 *bad 에 해당 키
bool      cfg_set_many(const char *const keys[], const char *const vals[], size_t n, const char **bad);
bool      cfg_is_saved(void);       // NVS 에 저장된 설정으로 동작 중 (false = 빌드 기본값)
void      cfg_confirm(void);        // 서버 통신 확인 → 원격 변경 설정 확정 (반복 호출 무해)
const char *cfg_state_str(void);    // saved | default | trial
