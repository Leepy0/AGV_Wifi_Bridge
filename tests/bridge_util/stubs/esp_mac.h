#include "host_stubs.h"
// MAC 문자열 매크로 (ESP-IDF esp_mac.h 와 같은 형식)
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
typedef int esp_mac_type_t;
#define ESP_MAC_WIFI_STA 0
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
