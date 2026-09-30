#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "app_config.h"

static const char *TAG = "cfg";
static const char *NVS_NS  = "bridge";
static const char *NVS_KEY = "cfg";

app_cfg_t g_cfg;                    // 실행 중 설정 — 부팅 시 확정, 이후 변경 안 함
static app_cfg_t s_edit;            // 콘솔 편집용 사본 — save 후 재부팅 시 반영
volatile bool g_monitor = false;

// 10진 dotted-quad 만 허용 (inet_aton 의 8진/축약 표기 오해석 방지)
static bool parse_ip(const char *s, uint32_t *out)
{
    unsigned int o[4];
    char tail;
    if (s == NULL || sscanf(s, "%3u.%3u.%3u.%3u%c", &o[0], &o[1], &o[2], &o[3], &tail) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (o[i] > 255) {
            return false;
        }
    }
    // 네트워크 바이트 순서로 저장
    *out = htonl(((uint32_t)o[0] << 24) | ((uint32_t)o[1] << 16) | ((uint32_t)o[2] << 8) | o[3]);
    return true;
}

// 빈 문자열 또는 키워드(dhcp/off) → 0
static bool parse_ip_or_zero(const char *s, const char *zero_word, uint32_t *out)
{
    if (s == NULL || s[0] == '\0' || strcmp(s, zero_word) == 0) {
        *out = 0;
        return true;
    }
    return parse_ip(s, out);
}

static bool parse_uint(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static const char *ip_str(uint32_t ip, char *buf, size_t len)
{
    struct in_addr a = { .s_addr = ip };
    inet_ntoa_r(a, buf, len);
    return buf;
}

static void set_str(char *dst, size_t cap, const char *src)
{
    memset(dst, 0, cap);
    strlcpy(dst, src, cap);
}

// Kconfig 값으로 기본 설정 구성
static void cfg_defaults(app_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = CFG_VERSION;
    c->agv_id  = CONFIG_BR_AGV_ID;
    set_str(c->ssid, sizeof(c->ssid), CONFIG_BR_WIFI_SSID);
    set_str(c->pass, sizeof(c->pass), CONFIG_BR_WIFI_PASS);
    parse_ip_or_zero(CONFIG_BR_STATIC_IP, "dhcp", &c->static_ip);
    parse_ip_or_zero(CONFIG_BR_NETMASK, "", &c->netmask);
    parse_ip_or_zero(CONFIG_BR_GATEWAY, "", &c->gateway);
    parse_ip_or_zero(CONFIG_BR_SERVER_IP, "", &c->server_ip);
    c->server_port   = CONFIG_BR_SERVER_PORT;
    c->local_port    = CONFIG_BR_LOCAL_PORT;
    parse_ip_or_zero(CONFIG_BR_RFID_DST_IP, "off", &c->rfid_dst_ip);
    c->rfid_dst_port = CONFIG_BR_RFID_DST_PORT;
    c->main_baud     = CONFIG_BR_MAIN_BAUD;
    c->rfid_baud     = CONFIG_BR_RFID_BAUD;
    c->hb_ms         = CONFIG_BR_HB_MS;
}

void cfg_load(void)
{
    nvs_handle_t h;
    app_cfg_t tmp;
    size_t len = sizeof(tmp);

    cfg_defaults(&g_cfg);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, NVS_KEY, &tmp, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(tmp) && tmp.version == CFG_VERSION) {
            g_cfg = tmp;
            ESP_LOGI(TAG, "loaded config from NVS");
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "saved config invalid (%s), using defaults", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "no saved config, using defaults");
    }
    s_edit = g_cfg;
}

esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, &s_edit, sizeof(s_edit));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cfg_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        cfg_defaults(&s_edit);
    }
    return err;
}

// 편집 사본 표시. 실행 중 설정과 다르면 안내
void cfg_print(void)
{
    const app_cfg_t *c = &s_edit;
    char a[16], b[16], d[16];

    printf("agv=%u ssid=\"%s\" pass=(%u chars)\n", c->agv_id, c->ssid, (unsigned)strlen(c->pass));
    printf("ip=%s mask=%s gw=%s\n",
           c->static_ip ? ip_str(c->static_ip, a, sizeof(a)) : "dhcp",
           ip_str(c->netmask, b, sizeof(b)), ip_str(c->gateway, d, sizeof(d)));
    printf("server=%s:%u local_port=%u\n", ip_str(c->server_ip, a, sizeof(a)), c->server_port, c->local_port);
    printf("rfid_dst=%s:%u\n", c->rfid_dst_ip ? ip_str(c->rfid_dst_ip, a, sizeof(a)) : "off", c->rfid_dst_port);
    printf("main_baud=%lu rfid_baud=%lu hb_ms=%u\n",
           (unsigned long)c->main_baud, (unsigned long)c->rfid_baud, c->hb_ms);
    if (memcmp(&s_edit, &g_cfg, sizeof(g_cfg)) != 0) {
        printf("* differs from running config - 'save' then 'reboot' to apply\n");
    }
}

// 콘솔 'set <key> <value>' — 편집 사본만 변경 (실행 중 동작에는 영향 없음)
bool cfg_set(const char *key, const char *val)
{
    app_cfg_t *c = &s_edit;
    uint32_t v, ip;

    if (strcmp(key, "agv") == 0) {
        if (!parse_uint(val, 1, 99, &v)) return false;
        c->agv_id = (uint8_t)v;
    } else if (strcmp(key, "ssid") == 0) {
        if (strlen(val) == 0 || strlen(val) >= sizeof(c->ssid)) return false;
        set_str(c->ssid, sizeof(c->ssid), val);
    } else if (strcmp(key, "pass") == 0) {
        // '-' = 비밀번호 없음(오픈 AP). WPA2 는 8~63자 또는 64자리 hex
        const char *p = strcmp(val, "-") == 0 ? "" : val;
        size_t n = strlen(p);
        if (n != 0 && (n < 8 || n > 64)) return false;
        set_str(c->pass, sizeof(c->pass), p);
    } else if (strcmp(key, "ip") == 0) {
        if (!parse_ip_or_zero(val, "dhcp", &ip)) return false;
        c->static_ip = ip;
    } else if (strcmp(key, "mask") == 0) {
        if (!parse_ip(val, &ip) || ip == 0) return false;
        c->netmask = ip;
    } else if (strcmp(key, "gw") == 0) {
        if (!parse_ip(val, &ip)) return false;
        c->gateway = ip;
    } else if (strcmp(key, "server") == 0) {
        if (!parse_ip(val, &ip) || ip == 0) return false;
        c->server_ip = ip;
    } else if (strcmp(key, "server_port") == 0) {
        if (!parse_uint(val, 1, 65535, &v)) return false;
        c->server_port = (uint16_t)v;
    } else if (strcmp(key, "local_port") == 0) {
        if (!parse_uint(val, 1, 65535, &v)) return false;
        c->local_port = (uint16_t)v;
    } else if (strcmp(key, "rfid_dst") == 0) {
        if (!parse_ip_or_zero(val, "off", &ip)) return false;
        c->rfid_dst_ip = ip;
    } else if (strcmp(key, "rfid_port") == 0) {
        if (!parse_uint(val, 1, 65535, &v)) return false;
        c->rfid_dst_port = (uint16_t)v;
    } else if (strcmp(key, "main_baud") == 0) {
        if (!parse_uint(val, 1200, 1000000, &v)) return false;
        c->main_baud = v;
    } else if (strcmp(key, "rfid_baud") == 0) {
        if (!parse_uint(val, 1200, 1000000, &v)) return false;
        c->rfid_baud = v;
    } else if (strcmp(key, "hb_ms") == 0) {
        if (!parse_uint(val, 0, 60000, &v) || (v > 0 && v < 100)) return false;
        c->hb_ms = (uint16_t)v;
    } else {
        return false;
    }
    return true;
}
