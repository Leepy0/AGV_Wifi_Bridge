#pragma once
#include <stdbool.h>
#include "esp_err.h"

// OTA 펌웨어 업데이트 + 롤백 관리
//  - 새 펌웨어로 부팅하면 "pending" 상태. 서버에서 하향 패킷을 받아 통신이 확인되면 confirm → 확정
//  - 확인 없이 타임아웃되면 이전 슬롯으로 롤백 재부팅 (Wi-Fi/서버 설정이 깨진 펌웨어 방지)
void        ota_init(void);                 // 부팅 시 호출
void        ota_confirm(void);              // 서버 통신 확인 → 현재 펌웨어 확정 (반복 호출 무해)
esp_err_t   ota_start(const char *url);     // URL 에서 받아 다른 슬롯에 기록 후 재부팅 (ESP_ERR_INVALID_STATE = 진행 중)
bool        ota_busy(void);
const char *ota_state_str(void);            // valid | pending | busy | none
const char *ota_running_partition(void);    // "ota_0" / "ota_1"
