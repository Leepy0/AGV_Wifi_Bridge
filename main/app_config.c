#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "bridge_util.h"

static const char *TAG = "cfg";
static const char *NVS_NS      = "bridge";
static const char *NVS_KEY     = "cfg";
static const char *NVS_KEY_BAK = "cfg_bak";     // 원격 저장 직전의 동작 설정 — 있으면 확인 대기(trial) 중

// CFG_VERSION 1 저장본과의 호환: DNS 필드를 추가하기 전의 구조
typedef struct {
    uint16_t version;
    uint8_t  agv_id;
    char     ssid[33];
    char     pass[65];
    uint32_t static_ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t server_ip;
    uint16_t server_port;
    uint16_t local_port;
    uint32_t rfid_dst_ip;
    uint16_t rfid_dst_port;
    uint32_t main_baud;
    uint32_t rfid_baud;
    uint16_t hb_ms;
} app_cfg_v1_t;

_Static_assert(offsetof(app_cfg_t, dns_primary) == sizeof(app_cfg_v1_t),
               "DNS fields must be appended to preserve v1 config layout");

static void migrate_v1(const app_cfg_v1_t *old, app_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out, old, sizeof(*old));
    out->version = CFG_VERSION;
}

#define TRIAL_TIMEOUT_US  (180LL * 1000 * 1000)  // 원격 변경 후 이 시간 안에 서버 통신이 없으면 복구

_Static_assert(CONFIG_BR_HB_MS == 0 || (CONFIG_BR_HB_MS >= BU_HB_MS_MIN && CONFIG_BR_HB_MS <= BU_HB_MS_MAX),
               "BR_HB_MS must be 0 (off) or 100-2000 ms");

app_cfg_t g_cfg;                    // 실행 중 설정 — 부팅 시 확정, 이후 변경 안 함
static app_cfg_t s_edit;            // 편집용 사본 (콘솔/원격/웹) — save 후 재부팅 시 반영
static SemaphoreHandle_t s_lock;    // s_edit·NVS 접근 보호
static bool s_saved;                // NVS 설정으로 동작 중 / 저장됨
static volatile bool s_trial;       // 원격 변경 설정 확인 대기 중
static esp_timer_handle_t s_trial_timer;
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
    parse_ip_or_zero(CONFIG_BR_DNS_PRIMARY, "auto", &c->dns_primary);
    parse_ip_or_zero(CONFIG_BR_DNS_SECONDARY, "auto", &c->dns_secondary);
}

// 원격 변경 후 서버 통신이 없음 → 백업해 둔 이전 설정으로 되돌리고 재부팅
static void trial_timeout(void *arg)
{
    static union { app_cfg_t current; app_cfg_v1_t legacy; } bak; // esp_timer 태스크 스택 절약
    size_t len = sizeof(bak);
    nvs_handle_t h;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_trial) {                 // 직전에 확정됨
        xSemaphoreGive(s_lock);
        return;
    }
    ESP_LOGE(TAG, "no server contact within %llds after remote config change - restoring previous config",
             TRIAL_TIMEOUT_US / 1000000);
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY_BAK, &bak, &len) == ESP_OK) {
            if (len == sizeof(app_cfg_t) && bak.current.version == CFG_VERSION) {
                nvs_set_blob(h, NVS_KEY, &bak.current, sizeof(bak.current));
            } else if (len == sizeof(app_cfg_v1_t) && bak.legacy.version == 1) {
                app_cfg_t migrated;
                migrate_v1(&bak.legacy, &migrated);
                nvs_set_blob(h, NVS_KEY, &migrated, sizeof(migrated));
            }
        }
        nvs_erase_key(h, NVS_KEY_BAK);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

// 확인 대기 해제 (NVS 백업 삭제는 호출 측). s_lock 보유 상태에서 호출
static void trial_cancel(void)
{
    s_trial = false;
    if (s_trial_timer) {
        esp_timer_stop(s_trial_timer);
    }
}

void cfg_load(void)
{
    nvs_handle_t h;
    union { app_cfg_t current; app_cfg_v1_t legacy; } stored = {0};
    size_t len = sizeof(stored);
    bool migrated = false;

    s_lock = xSemaphoreCreateMutex();
    cfg_defaults(&g_cfg);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_blob(h, NVS_KEY, &stored, &len);
        size_t bak_len = 0;
        bool bak = nvs_get_blob(h, NVS_KEY_BAK, NULL, &bak_len) == ESP_OK;
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(app_cfg_t) && stored.current.version == CFG_VERSION) {
            g_cfg = stored.current;
            s_saved = true;
            ESP_LOGI(TAG, "loaded config from NVS");
            if (!bu_hb_ms_valid(g_cfg.hb_ms)) {
                // 이전 버전에서 허용하던 값(최대 60000) — 서버의 1초 주기 전제와 맞지 않아 기본값으로
                ESP_LOGW(TAG, "saved hb_ms %u out of range (0 or %d-%d) - using default %d",
                         g_cfg.hb_ms, BU_HB_MS_MIN, BU_HB_MS_MAX, CONFIG_BR_HB_MS);
                g_cfg.hb_ms = CONFIG_BR_HB_MS;
            }
        } else if (err == ESP_OK && len == sizeof(app_cfg_v1_t) && stored.legacy.version == 1) {
            migrate_v1(&stored.legacy, &g_cfg);
            s_saved = true;
            migrated = true;
            ESP_LOGI(TAG, "migrated saved config v1 -> v2 (DNS defaults to automatic)");
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "saved config invalid (%s), using defaults", esp_err_to_name(err));
        }
        if (migrated) {
            nvs_handle_t wh;
            if (nvs_open(NVS_NS, NVS_READWRITE, &wh) == ESP_OK) {
                esp_err_t we = nvs_set_blob(wh, NVS_KEY, &g_cfg, sizeof(g_cfg));
                if (we == ESP_OK) we = nvs_commit(wh);
                if (we != ESP_OK) ESP_LOGW(TAG, "config migration save failed: %s", esp_err_to_name(we));
                nvs_close(wh);
            }
        }
        if (bak) {
            // 서버 원격 저장 후 첫 부팅 — 서버 통신이 확인되면 확정, 아니면 이전 설정으로 복구
            const esp_timer_create_args_t ta = { .callback = trial_timeout, .name = "cfg_trial" };
            if (esp_timer_create(&ta, &s_trial_timer) == ESP_OK) {
                s_trial = true;
                esp_timer_start_once(s_trial_timer, TRIAL_TIMEOUT_US);
                ESP_LOGW(TAG, "remote config change - pending verify (revert if no server contact in %llds)",
                         TRIAL_TIMEOUT_US / 1000000);
            }
        }
    } else {
        ESP_LOGI(TAG, "no saved config, using defaults");
    }
    s_edit = g_cfg;
}

esp_err_t cfg_save(bool trial)
{
    nvs_handle_t h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    if (trial) {
        // 지금 동작 중인 설정(서버와 통신이 된 설정)을 백업 — 재부팅 후 서버 통신이 없으면 이것으로 복구
        err = nvs_set_blob(h, NVS_KEY_BAK, &g_cfg, sizeof(g_cfg));
    } else {
        // 현장(콘솔·설정 AP)에서 저장 = 작업자가 확인한 설정 → 대기 중인 복구 취소
        err = nvs_erase_key(h, NVS_KEY_BAK);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_edit, sizeof(s_edit));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_saved = true;
        if (!trial) {
            trial_cancel();
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t cfg_erase(void)
{
    nvs_handle_t h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_erase_key(h, NVS_KEY_BAK);
    }
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        cfg_defaults(&s_edit);
        s_saved = false;
        trial_cancel();
    }
    xSemaphoreGive(s_lock);
    return err;
}

void cfg_confirm(void)
{
    if (!s_trial) {
        return;
    }
    nvs_handle_t h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_trial && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_erase_key(h, NVS_KEY_BAK);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err == ESP_OK) {
            trial_cancel();
            ESP_LOGI(TAG, "remote config confirmed");
        }
    }
    xSemaphoreGive(s_lock);
}

bool cfg_is_saved(void)
{
    return s_saved;
}

const char *cfg_state_str(void)
{
    if (s_trial) return "trial";
    return s_saved ? "saved" : "default";
}

void cfg_get_edit(app_cfg_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_edit, sizeof(*out));     // 패딩까지 복사 — cfg_format 의 memcmp 비교용
    xSemaphoreGive(s_lock);
}

// 편집 사본 내용 (콘솔 cfg / 서버 원격 cfg 공용). 실행 중 설정과 다르면 안내
size_t cfg_format(char *buf, size_t cap)
{
    app_cfg_t c;
    char a[16], b[16], d[16];
    size_t n = 0;

    cfg_get_edit(&c);
#define OUT(...) do { if (n < cap) { int r = snprintf(buf + n, cap - n, __VA_ARGS__); if (r > 0) n += (size_t)r; } } while (0)
    OUT("agv=%u ssid=\"%s\" pass=(%u chars)\n", c.agv_id, c.ssid, (unsigned)strlen(c.pass));
    OUT("ip=%s mask=%s gw=%s\n",
        c.static_ip ? ip_str(c.static_ip, a, sizeof(a)) : "dhcp",
        ip_str(c.netmask, b, sizeof(b)), ip_str(c.gateway, d, sizeof(d)));
    OUT("dns=%s dns2=%s\n",
        c.dns_primary ? ip_str(c.dns_primary, a, sizeof(a)) : "auto",
        c.dns_secondary ? ip_str(c.dns_secondary, b, sizeof(b)) : "auto");
    OUT("server=%s:%u local_port=%u\n", ip_str(c.server_ip, a, sizeof(a)), c.server_port, c.local_port);
    OUT("rfid_dst=%s:%u\n", c.rfid_dst_ip ? ip_str(c.rfid_dst_ip, a, sizeof(a)) : "off", c.rfid_dst_port);
    OUT("main_baud=%lu rfid_baud=%lu hb_ms=%u\n",
        (unsigned long)c.main_baud, (unsigned long)c.rfid_baud, c.hb_ms);
    OUT("state=%s\n", cfg_state_str());
    if (memcmp(&c, &g_cfg, sizeof(g_cfg)) != 0) {
        OUT("* differs from running config - 'save' then 'reboot' to apply\n");
    }
#undef OUT
    return n < cap ? n : (cap ? cap - 1 : 0);
}

void cfg_print(void)
{
    char buf[400];
    cfg_format(buf, sizeof(buf));
    fputs(buf, stdout);
}

// 키 하나를 c 에 적용 (값 검증 포함)
static bool apply(app_cfg_t *c, const char *key, const char *val)
{
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
    } else if (strcmp(key, "dns") == 0) {
        if (!parse_ip_or_zero(val, "auto", &ip)) return false;
        c->dns_primary = ip;
    } else if (strcmp(key, "dns2") == 0) {
        if (!parse_ip_or_zero(val, "auto", &ip)) return false;
        c->dns_secondary = ip;
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
        if (!parse_uint(val, 0, BU_HB_MS_MAX, &v) || !bu_hb_ms_valid(v)) return false;   // 0 = 끔, 100~2000
        c->hb_ms = (uint16_t)v;
    } else {
        return false;
    }
    return true;
}

// 'set <key> <value>' — 편집 사본만 변경 (실행 중 동작에는 영향 없음)
bool cfg_set(const char *key, const char *val)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = apply(&s_edit, key, val);
    xSemaphoreGive(s_lock);
    return ok;
}

bool cfg_set_many(const char *const keys[], const char *const vals[], size_t n, const char **bad)
{
    static app_cfg_t tmp;           // s_lock 보호 — 호출 태스크 스택 절약
    bool ok = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    tmp = s_edit;
    for (size_t i = 0; i < n && ok; i++) {
        if (!apply(&tmp, keys[i], vals[i])) {
            ok = false;
            if (bad) {
                *bad = keys[i];
            }
        }
    }
    if (ok) {
        s_edit = tmp;
    }
    xSemaphoreGive(s_lock);
    return ok;
}
