// AGV ZigBee 통신보드 대체용 Wi-Fi 브리지 (ESP32-S3 + MAX3232)
//  - UART1 ↔ 메인보드 J4 (TMS320F2808): 프레임 그대로 서버와 UDP 중계
//  - UART2 ← RFID 리더: 메인보드로 바이패스 + 네트워크 브로드캐스트
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "uart_bridge.h"
#include "net_bridge.h"
#include "console_cmd.h"
#include "ota.h"

static const char *TAG = "main";

// 메인보드(J4) 수신 프레임 → 서버
static void on_main_frame(const uint8_t *data, size_t len)
{
    net_send_main(data, len);
    if (g_monitor) {    // 벤치 확인용 — 전달 후 출력해 지연에 영향 없게
        ESP_LOG_BUFFER_HEXDUMP("MAIN>", data, len, ESP_LOG_INFO);
    }
}

// RFID 리더 수신 프레임 → 메인보드 바이패스 + 네트워크
static void on_rfid_frame(const uint8_t *data, size_t len)
{
#if CONFIG_BR_RFID_TO_MAIN
    uart_bridge_send_main(data, len);   // 주행 제어에 직결되므로 먼저 처리, Wi-Fi 상태와 무관
#endif
    net_send_rfid(data, len);
    if (g_monitor) {
        ESP_LOG_BUFFER_HEXDUMP("RFID>", data, len, ESP_LOG_INFO);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    cfg_load();
    cfg_print();
    ota_init();     // 새 펌웨어면 롤백 대기 시작 (서버 패킷 수신 시 확정)

    // UART 를 먼저 올려 Wi-Fi 가 안 붙어도 RFID → 메인보드 바이패스는 동작
    ESP_ERROR_CHECK(uart_bridge_start(on_main_frame, on_rfid_frame));

    err = net_bridge_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network start failed: %s (RFID bypass still active)", esp_err_to_name(err));
    }

    console_start();
}
