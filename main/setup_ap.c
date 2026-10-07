#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "net_bridge.h"
#include "ota.h"
#include "setup_ap.h"

static const char *TAG = "setup";

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define BR_WIFI_BW20 WIFI_BW20
#else
#define BR_WIFI_BW20 WIFI_BW_HT20
#endif

#define AP_CHANNEL      1           // STA 가 연결돼 있으면 STA 채널을 따라감
#define AP_MAX_CONN     2
#define TASK_PRIO       2           // 중계(UART/net) 태스크보다 낮게
#define TICK_MS         100
#define BTN_HOLD_MS     3000
#define OPEN_RETRY_US   (1000LL * 1000)
#define IDLE_TIMEOUT_US ((int64_t)CONFIG_BR_SETUP_AP_TIMEOUT_S * 1000 * 1000)
#define BODY_MAX        1024
#define FIELD_MAX       16

_Static_assert(sizeof(CONFIG_BR_SETUP_AP_PASS) == 1 ||
               (sizeof(CONFIG_BR_SETUP_AP_PASS) >= 9 && sizeof(CONFIG_BR_SETUP_AP_PASS) <= 64),
               "setup AP password must be empty (open) or 8-63 chars");

static esp_netif_t      *s_ap_netif;
static wifi_config_t     s_ap_cfg;
static httpd_handle_t    s_httpd;
static bool              s_ready;           // 관리 태스크 동작 중
static volatile bool     s_want;            // 열기 요청 (false = 닫기)
static volatile bool     s_on;              // AP 동작 중
static volatile bool     s_reboot;          // 웹 저장 후 재부팅 예약
static volatile int      s_clients;         // AP 접속 단말 수
static volatile int64_t  s_idle_since;      // 단말이 없어진 시각

// ───────── 웹 페이지 ─────────

// 입력 항목 (sect != NULL 이면 새 묶음 시작). 키는 콘솔 'set' 과 동일
static const struct {
    const char *sect, *key, *label, *attr;
} FIELDS[] = {
    { "호기",   "agv",         "AGV ID (1-99)",                     "type=number min=1 max=99 required" },
    { "Wi-Fi",  "ssid",        "SSID",                              "maxlength=32 required autocapitalize=off autocorrect=off" },
    { NULL,     "pass",        "비밀번호",                          "type=password maxlength=64 autocomplete=new-password" },
    { NULL,     "ip",          "고정 IP (비우면 DHCP)",             "inputmode=decimal placeholder=DHCP" },
    { NULL,     "mask",        "서브넷 마스크",                     "inputmode=decimal required" },
    { NULL,     "gw",          "게이트웨이",                        "inputmode=decimal required" },
    { "서버",   "server",      "서버 IP",                           "inputmode=decimal required" },
    { NULL,     "server_port", "서버 포트",                         "type=number min=1 max=65535 required" },
    { NULL,     "local_port",  "수신 포트",                         "type=number min=1 max=65535 required" },
    { NULL,     "rfid_dst",    "RFID 송신 IP (255.255.255.255 = 브로드캐스트, 비우면 끔)", "inputmode=decimal placeholder=off" },
    { NULL,     "rfid_port",   "RFID 송신 포트",                    "type=number min=1 max=65535 required" },
    { "시리얼", "main_baud",   "메인보드 보드레이트",               "type=number min=1200 max=1000000 required" },
    { NULL,     "rfid_baud",   "RFID 보드레이트",                   "type=number min=1200 max=1000000 required" },
    { NULL,     "hb_ms",       "상태 보고 주기 ms (0 = 끔)",        "type=number min=0 max=60000 required" },
};

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang=ko><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>AGV Bridge 설정</title><style>"
    "body{font-family:sans-serif;margin:0 auto;padding:16px;max-width:520px;color:#222}"
    "h1{font-size:20px;margin:0 0 8px}.st{font-size:14px;color:#444;line-height:1.6;margin:0 0 12px}"
    "fieldset{border:1px solid #bbb;border-radius:8px;margin:0 0 12px;padding:4px 12px 12px}"
    "legend{font-weight:bold;padding:0 4px}label{display:block;margin-top:10px;font-size:14px}"
    "input{display:block;width:100%;box-sizing:border-box;margin-top:4px;padding:10px;font-size:16px;min-height:44px}"
    "button{width:100%;min-height:52px;font-size:17px;font-weight:bold;margin-top:4px}"
    ".warn{color:#b00}</style></head><body><h1>AGV Wi-Fi Bridge 설정</h1>";

static esp_err_t sendf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

// HTML 속성값용 이스케이프 (SSID 에 따옴표·꺾쇠가 있어도 안전)
static const char *html_esc(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        const char *r = NULL;
        switch (*s) {
        case '&':  r = "&amp;";  break;
        case '<':  r = "&lt;";   break;
        case '>':  r = "&gt;";   break;
        case '"':  r = "&quot;"; break;
        case '\'': r = "&#39;";  break;
        default:   break;
        }
        if (r) {
            size_t l = strlen(r);
            memcpy(out + n, r, l);
            n += l;
        } else {
            out[n++] = *s;
        }
    }
    out[n] = '\0';
    return out;
}

// application/x-www-form-urlencoded 값 복원 (제자리)
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], '\0' };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        s[--n] = '\0';
    }
    return s;
}

static const char *ip_or(uint32_t ip, const char *zero, char *buf, size_t len)
{
    if (ip == 0) {
        return zero;
    }
    struct in_addr a = { .s_addr = ip };
    inet_ntoa_r(a, buf, len);
    return buf;
}

// 입력 항목의 현재 값 (편집 사본 기준, 비밀번호는 표시하지 않음)
static const char *field_value(const app_cfg_t *c, const char *key, char *buf, size_t len)
{
    if (strcmp(key, "agv") == 0)         snprintf(buf, len, "%u", c->agv_id);
    else if (strcmp(key, "ssid") == 0)   return c->ssid;
    else if (strcmp(key, "ip") == 0)     return ip_or(c->static_ip, "", buf, len);
    else if (strcmp(key, "mask") == 0)   return ip_or(c->netmask, "0.0.0.0", buf, len);
    else if (strcmp(key, "gw") == 0)     return ip_or(c->gateway, "0.0.0.0", buf, len);
    else if (strcmp(key, "server") == 0) return ip_or(c->server_ip, "", buf, len);
    else if (strcmp(key, "server_port") == 0) snprintf(buf, len, "%u", c->server_port);
    else if (strcmp(key, "local_port") == 0)  snprintf(buf, len, "%u", c->local_port);
    else if (strcmp(key, "rfid_dst") == 0)    return ip_or(c->rfid_dst_ip, "", buf, len);
    else if (strcmp(key, "rfid_port") == 0)   snprintf(buf, len, "%u", c->rfid_dst_port);
    else if (strcmp(key, "main_baud") == 0)   snprintf(buf, len, "%lu", (unsigned long)c->main_baud);
    else if (strcmp(key, "rfid_baud") == 0)   snprintf(buf, len, "%lu", (unsigned long)c->rfid_baud);
    else if (strcmp(key, "hb_ms") == 0)       snprintf(buf, len, "%u", c->hb_ms);
    else return "";
    return buf;
}

// 설정 AP 로 들어온 요청만 허용 — 공장망(STA) 쪽 IP 로는 이 페이지에 접근 불가
static bool from_ap(httpd_req_t *req)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    esp_netif_ip_info_t ip;
    uint32_t local;

    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&sa, &len) != 0 ||
        esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK) {
        return false;
    }
    if (sa.ss_family == AF_INET) {
        local = ((struct sockaddr_in *)&sa)->sin_addr.s_addr;
#if CONFIG_LWIP_IPV6
    } else if (sa.ss_family == AF_INET6) {
        // httpd 는 IPv6 소켓 — IPv4 접속은 ::ffff:a.b.c.d 로 보임
        memcpy(&local, &((struct sockaddr_in6 *)&sa)->sin6_addr.s6_addr[12], sizeof(local));
#endif
    } else {
        return false;
    }
    return local == ip.ip.addr;
}

static esp_err_t page_msg(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    sendf(req, "<p>%s</p>", msg);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_root(httpd_req_t *req)
{
    char v[24], esc[200];
    app_cfg_t c;

    if (!from_ap(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "setup AP only");
    }
    cfg_get_edit(&c);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);

    // 상태
    sendf(req, "<p class=st>펌웨어 %s (%s, %s) · 설정 %s<br>",
          esp_app_get_description()->version, ota_running_partition(), ota_state_str(), cfg_state_str());
    if (net_is_up()) {
        esp_netif_ip_info_t ip = {0};
        wifi_ap_record_t ap = {0};
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
        esp_wifi_sta_get_ap_info(&ap);
        sendf(req, "Wi-Fi 연결됨: " IPSTR " · RSSI %d dBm · ch %u</p>", IP2STR(&ip.ip), ap.rssi, ap.primary);
    } else {
        sendf(req, "<span class=warn>Wi-Fi 미연결 (\"%s\")</span></p>", html_esc(g_cfg.ssid, esc, sizeof(esc)));
    }

    sendf(req, "<form method=post action=/save>");
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        if (FIELDS[i].sect) {
            sendf(req, "%s<fieldset><legend>%s</legend>", i ? "</fieldset>" : "", FIELDS[i].sect);
        }
        if (strcmp(FIELDS[i].key, "pass") == 0) {
            // 비밀번호는 내려보내지 않음 — 비우면 기존 값 유지
            sendf(req, "<label>%s (현재 %u자 · 비우면 유지 · - 입력 시 없음)<input name=pass %s></label>",
                  FIELDS[i].label, (unsigned)strlen(c.pass), FIELDS[i].attr);
            continue;
        }
        sendf(req, "<label>%s<input name=%s value=\"%s\" %s></label>", FIELDS[i].label, FIELDS[i].key,
              html_esc(field_value(&c, FIELDS[i].key, v, sizeof(v)), esc, sizeof(esc)), FIELDS[i].attr);
    }
    sendf(req, "</fieldset><button>저장 후 재부팅</button></form>"
               "<p class=st>저장하면 재부팅하며 설정 AP 는 닫힙니다. 고정 IP 사용 시 마스크·게이트웨이도 확인하세요.</p>"
               "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t h_save(httpd_req_t *req)
{
    static char body[BODY_MAX + 1];     // httpd 태스크 하나에서만 실행
    const char *keys[FIELD_MAX], *vals[FIELD_MAX];
    size_t got = 0, n = 0;

    if (!from_ap(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "setup AP only");
    }
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    body[got] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(body, "&", &save); tok && n < FIELD_MAX; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *val = eq + 1;
        url_decode(tok);
        url_decode(val);
        if (strcmp(tok, "pass") == 0) {
            if (val[0] == '\0') {
                continue;               // 비우면 기존 비밀번호 유지
            }
        } else if (strcmp(tok, "ssid") != 0) {
            val = trim(val);            // 휴대폰 자동 입력 공백 제거 (SSID·비밀번호는 그대로)
        }
        keys[n] = tok;
        vals[n] = val;
        n++;
    }

    const char *bad = NULL;
    if (n == 0 || !cfg_set_many(keys, vals, n, &bad)) {
        const char *label = bad ? bad : "-";
        for (size_t i = 0; bad && i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
            if (strcmp(FIELDS[i].key, bad) == 0) {
                label = FIELDS[i].label;
            }
        }
        char msg[200];
        snprintf(msg, sizeof(msg), "<span class=warn>잘못된 값: %s</span> — 저장하지 않았습니다. "
                 "<a href=/>돌아가기</a>", label);
        return page_msg(req, "400 Bad Request", msg);
    }
    esp_err_t err = cfg_save(false);
    if (err != ESP_OK) {
        char msg[120];
        snprintf(msg, sizeof(msg), "<span class=warn>저장 실패: %s</span> <a href=/>돌아가기</a>", esp_err_to_name(err));
        return page_msg(req, "500 Internal Server Error", msg);
    }
    ESP_LOGI(TAG, "config saved from setup page - rebooting");
    s_reboot = true;                    // 응답을 보낸 뒤 관리 태스크가 재부팅
    return page_msg(req, "200 OK", "저장했습니다. 재부팅하면 새 설정으로 동작하고 설정 AP 는 닫힙니다.");
}

static const httpd_uri_t URI_ROOT = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
static const httpd_uri_t URI_SAVE = { .uri = "/save", .method = HTTP_POST, .handler = h_save };

// ───────── AP 열기/닫기 ─────────

void setup_ap_prepare(void)
{
    uint8_t mac[6] = {0};
    wifi_ap_config_t *ap = &s_ap_cfg.ap;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    int n = snprintf((char *)ap->ssid, sizeof(ap->ssid), "AGVBR-%02u-%02X%02X", g_cfg.agv_id, mac[4], mac[5]);
    ap->ssid_len       = (uint8_t)n;
    strlcpy((char *)ap->password, CONFIG_BR_SETUP_AP_PASS, sizeof(ap->password));
    ap->authmode       = ap->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap->channel        = AP_CHANNEL;
    ap->max_connection = AP_MAX_CONN;

    // AP 설정을 미리 등록 — 나중에 APSTA 로 바꿀 때 기본값(ESP_xxxx, 암호 없음) AP 가 잠깐 뜨지 않게.
    // 이후 net_bridge 가 STA 모드로 되돌린 뒤 esp_wifi_start
    if (s_ap_netif == NULL || esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "setup AP pre-config failed");
    } else {
        // STA 와 같이 20MHz — 혼잡한 공장 채널에서 40MHz 로 주변 AP 와 겹치지 않게
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_bandwidth(WIFI_IF_AP, BR_WIFI_BW20));
    }
}

static esp_err_t ap_open(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);   // 사전 등록이 유지되지 않는 경우 대비 (같은 값)
        if (err == ESP_OK) {
            esp_wifi_set_bandwidth(WIFI_IF_AP, BR_WIFI_BW20);
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "setup AP start failed: %s - retrying", esp_err_to_name(err));
        return err;
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.core_id          = 0;            // core 1 은 UART 중계용
    hc.stack_size       = 6144;
    hc.max_open_sockets = 3;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        httpd_register_uri_handler(s_httpd, &URI_ROOT);
        httpd_register_uri_handler(s_httpd, &URI_SAVE);
    } else {
        ESP_LOGE(TAG, "http server start failed");
        s_httpd = NULL;
    }

    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip);
    s_clients    = 0;
    s_idle_since = esp_timer_get_time();
    s_on         = true;
    ESP_LOGW(TAG, "setup AP \"%s\" open - http://" IPSTR, (const char *)s_ap_cfg.ap.ssid, IP2STR(&ip.ip));
    return ESP_OK;
}

static void ap_close(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_on      = false;
    s_clients = 0;
    ESP_LOGI(TAG, "setup AP closed");
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        s_clients++;
        ESP_LOGI(TAG, "client " MACSTR " joined (%d)", MAC2STR(e->mac), s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        if (s_clients > 0 && --s_clients == 0) {
            s_idle_since = esp_timer_get_time();
        }
        ESP_LOGI(TAG, "client " MACSTR " left (%d)", MAC2STR(e->mac), s_clients);
    }
}

// AP 열기/닫기·버튼·유휴 종료·웹 저장 후 재부팅을 한 태스크에서 처리 (Wi-Fi 모드 변경을 한곳으로)
static void setup_task(void *arg)
{
    int64_t next_try = 0;
#if CONFIG_BR_SETUP_BTN_GPIO >= 0
    uint32_t btn_ms = 0;
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();

        if (s_reboot) {
            vTaskDelay(pdMS_TO_TICKS(1000));    // 응답 페이지 전송 여유
            esp_restart();
        }

#if CONFIG_BR_SETUP_BTN_GPIO >= 0
        // 설정 버튼 3초 → AP 열기
        if (gpio_get_level(CONFIG_BR_SETUP_BTN_GPIO) == 0) {
            btn_ms += TICK_MS;
            if (btn_ms == BTN_HOLD_MS) {
                ESP_LOGI(TAG, "setup button held");
                setup_ap_request(true);
            }
        } else {
            btn_ms = 0;
        }
#endif

        if (s_want && !s_on && now >= next_try) {
            if (ap_open() != ESP_OK) {
                next_try = now + OPEN_RETRY_US;
            }
        } else if (!s_want && s_on) {
            ap_close();
        }

        // 단말 없이 STA 연결 상태가 timeout 지속 → 닫음. STA 미연결이면 유일한 접근 경로이므로 유지
        if (s_on && s_clients == 0 && net_is_up() && now - s_idle_since >= IDLE_TIMEOUT_US) {
            ESP_LOGI(TAG, "setup AP idle for %ds", CONFIG_BR_SETUP_AP_TIMEOUT_S);
            s_want = false;
            ap_close();
        }

        // 단말이 설정 페이지를 쓰는 동안 STA 재접속(전 채널 스캔)을 멈춰 AP 채널을 유지
        net_sta_hold(s_on && s_clients > 0 && !net_is_up());
    }
}

esp_err_t setup_ap_init(void)
{
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_ERR_INVALID_STATE, TAG, "AP netif");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event, NULL),
                        TAG, "ap evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event, NULL),
                        TAG, "ap evt");
#if CONFIG_BR_SETUP_BTN_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BR_SETUP_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "button");
#endif
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(setup_task, "setup", 4096, NULL, TASK_PRIO, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "setup task");
    s_ready = true;
    return ESP_OK;
}

void setup_ap_request(bool open)
{
    if (open && s_on) {
        s_idle_since = esp_timer_get_time();    // 이미 열려 있으면 유휴 시간만 연장
    }
    s_want = open;
}

bool setup_ap_active(void)
{
    return s_on;
}

size_t setup_ap_format(char *buf, size_t cap)
{
    int n;
    if (!s_ready) {
        n = snprintf(buf, cap, "setup AP: unavailable (Wi-Fi not started)\n");
    } else if (s_on) {
        n = snprintf(buf, cap, "setup AP: on ssid=%s clients=%d%s\n", (const char *)s_ap_cfg.ap.ssid, s_clients,
                     s_want ? "" : " (closing)");
    } else {
        n = snprintf(buf, cap, "setup AP: %s ssid=%s\n", s_want ? "opening" : "off", (const char *)s_ap_cfg.ap.ssid);
    }
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}
