#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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
#include "lwip/sockets.h"
#include "app_config.h"
#include "proto.h"
#include "uart_bridge.h"
#include "net_bridge.h"
#include "ota.h"
#include "setup.h"
#include "cmd.h"
#include "dsp_ota.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define BR_WIFI_BW20 WIFI_BW20
#else
#define BR_WIFI_BW20 WIFI_BW_HT20
#endif

static const char *TAG = "net";

#define BIT_UP          BIT0
#define RX_TASK_PRIO    11
#define HB_TASK_PRIO    3
#define DSCP_EF_TOS     0xB8    // DSCP 46(EF) — 유선 구간 QoS 마킹 (AP/스위치 설정에 따라 효과)
#define RECONN_FAST_TRIES 5     // 연속 실패 이 횟수까지는 즉시 재접속, 이후 RECONN_SLOW_MS 간격
#define RECONN_SLOW_MS  1000
#define CMD_LINE_MAX    200

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static int s_sock = -1;
static net_stat_t s_st;
// 타입별 순번 — 각 타입은 한 태스크에서만 송신하므로 잠금 불필요 (CFG_RESULT 는 요청 seq 반송)
static uint16_t s_seq_main, s_seq_rfid, s_seq_status, s_seq_ota, s_seq_dsp, s_seq_info;
static esp_timer_handle_t s_reconn_timer;
static uint32_t s_fail_streak;          // 연속 접속 실패 횟수 (재접속 간격 조절용)
static bool s_disconnect_logged;        // 연결 복구 전까지 disconnect 경고는 1회만 출력
static volatile bool s_hold;            // STA 재접속 보류 (설정 AP 사용 중)

bool net_is_up(void)
{
    return s_evt != NULL && (xEventGroupGetBits(s_evt) & BIT_UP);
}

static bool send_pkt(uint32_t ip, uint16_t port, uint8_t type, uint16_t seq,
                     const uint8_t *payload, size_t len)
{
    uint8_t buf[BR_HDR_LEN + BR_PAYLOAD_MAX];

    // 오프라인이면 버퍼링하지 않고 폐기 — 지난 교통제어 패킷이 늦게 도착하는 것보다 메인보드 재전송이 안전
    if (!net_is_up()) {
        s_st.tx_offline++;
        return false;
    }
    size_t n = br_build(buf, sizeof(buf), type, g_cfg.agv_id, seq, payload, len);
    if (n == 0) {
        s_st.tx_err++;
        return false;
    }
    struct sockaddr_in to = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = ip,
    };
    if (sendto(s_sock, buf, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        s_st.tx_err++;
        return false;
    }
    s_st.tx_pkts++;
    return true;
}

bool net_send_main(const uint8_t *data, size_t len)
{
    // 오프라인이어도 순번은 증가 → 서버에서 seq 누락으로 손실 구간 확인 가능
    return send_pkt(g_cfg.server_ip, g_cfg.server_port, BR_T_MAIN_UP, s_seq_main++, data, len);
}

bool net_send_rfid(const uint8_t *data, size_t len)
{
    if (g_cfg.rfid_dst_ip == 0) {
        return false;
    }
    return send_pkt(g_cfg.rfid_dst_ip, g_cfg.rfid_dst_port, BR_T_RFID, s_seq_rfid++, data, len);
}

bool net_send_ota_result(const uint8_t *data, size_t len)
{
    return send_pkt(g_cfg.server_ip, g_cfg.server_port, BR_T_OTA_RESULT, s_seq_ota++, data, len);
}

bool net_send_dsp_result(const uint8_t *data, size_t len)
{
    return send_pkt(g_cfg.server_ip, g_cfg.server_port, BR_T_DSP_OTA_RESULT, s_seq_dsp++, data, len);
}

// 장치 정보 (관리 서버 목록용): 설정·버전·메인보드 버전·스테이징 상태
static void send_info(const struct sockaddr_in *to)
{
    static char js[BR_PAYLOAD_MAX];     // net_rx 태스크 전용
    esp_netif_ip_info_t ip = {0};
    uint8_t mac[6] = {0};
    esp_netif_get_ip_info(s_netif, &ip);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    int n = snprintf(js, sizeof(js),
                     "{\"agv\":%u,\"mac\":\"" MACSTR "\",\"ip\":\"" IPSTR "\",\"fw\":\"%s\",\"part\":\"%s\",\"ota\":\"%s\","
                     "\"cfg\":\"%s\",\"setup\":%d,\"up\":%lld,",
                     g_cfg.agv_id, MAC2STR(mac), IP2STR(&ip.ip), esp_app_get_description()->version,
                     ota_running_partition(), ota_state_str(), cfg_state_str(), setup_active(),
                     (long long)(esp_timer_get_time() / 1000000));
    if (n > 0 && (size_t)n < sizeof(js)) {
        n += dsp_ota_status_json(js + n, sizeof(js) - n);
        n += snprintf(js + n, sizeof(js) - n, "}");
    }
    if (n > 0 && (size_t)n < sizeof(js)) {
        send_pkt(to->sin_addr.s_addr, ntohs(to->sin_port), BR_T_INFO, s_seq_info++, (const uint8_t *)js, (size_t)n);
    }
}

static void reconn_timer_cb(void *arg)
{
    if (!s_hold) {
        esp_wifi_connect();
    }
}

void net_sta_hold(bool hold)
{
    if (hold == s_hold) {
        return;
    }
    s_hold = hold;
    if (hold) {
        if (s_reconn_timer) {
            esp_timer_stop(s_reconn_timer);
        }
        if (!net_is_up()) {
            esp_wifi_disconnect();      // 진행 중인 접속 시도(스캔) 중단
        }
        ESP_LOGI(TAG, "reconnect on hold (setup AP in use)");
    } else if (!net_is_up()) {
        s_fail_streak = 0;
        esp_wifi_connect();
    }
}

// 고정 IP: DHCP 를 연결 전에 한 번 멈춰 두면 연결 시 esp_netif 가 이 값으로 GOT_IP 를 발생시킴
static esp_err_t set_static_ip(void)
{
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = g_cfg.static_ip;
    ip.netmask.addr = g_cfg.netmask;
    ip.gw.addr      = g_cfg.gateway;
    return esp_netif_set_ip_info(s_netif, &ip);
}

// 수동 DNS 가 설정된 경우에만 덮어씀. 0은 DHCP/자동 DNS를 그대로 사용한다.
static void apply_dns_config(void)
{
    const struct {
        esp_netif_dns_type_t type;
        uint32_t ip;
        const char *name;
    } entries[] = {
        { ESP_NETIF_DNS_MAIN,   g_cfg.dns_primary,   "primary" },
        { ESP_NETIF_DNS_BACKUP, g_cfg.dns_secondary, "secondary" },
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (entries[i].ip == 0) {
            continue;
        }
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = entries[i].ip;
        esp_err_t err = esp_netif_set_dns_info(s_netif, entries[i].type, &dns);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set %s DNS failed: %s", entries[i].name, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "%s DNS set to " IPSTR, entries[i].name, IP2STR(&dns.ip.u_addr.ip4));
        }
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = data;
        ESP_LOGI(TAG, "associated ch=%d bssid=" MACSTR, e->channel, MAC2STR(e->bssid));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        xEventGroupClearBits(s_evt, BIT_UP);
        s_st.disconnects++;
        s_st.last_reason = e->reason;
        if (!s_disconnect_logged) {
            ESP_LOGW(TAG, "disconnected reason=%d rssi=%d (further disconnect logs suppressed until Wi-Fi reconnects)",
                     e->reason, e->rssi);
            s_disconnect_logged = true;
        }
        // 로밍(BTM/FT) 중에는 드라이버가 직접 재접속하므로 개입하지 않음. 설정 AP 사용 중에는 보류
        if (e->reason != WIFI_REASON_ROAMING && !s_hold) {
            // 처음 몇 번은 즉시 재접속(이동 중 AP 이탈 대응), 계속 실패하면(AP 꺼짐·비밀번호 오류) 간격을 둠
            if (++s_fail_streak <= RECONN_FAST_TRIES) {
                esp_wifi_connect();
            } else if (s_reconn_timer) {
                esp_timer_start_once(s_reconn_timer, RECONN_SLOW_MS * 1000);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        apply_dns_config();            // DHCP가 DNS를 설정한 뒤 수동 지정값을 우선 적용
        s_fail_streak = 0;
        s_disconnect_logged = false;   // 연결 복구 후 다음 장애는 다시 1회 기록
        xEventGroupSetBits(s_evt, BIT_UP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "lost ip");
        xEventGroupClearBits(s_evt, BIT_UP);
    }
}

static esp_err_t wifi_init(void)
{
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "sta netif");
    if (g_cfg.static_ip != 0) {
        ESP_RETURN_ON_ERROR(set_static_ip(), TAG, "static ip");
    }
    apply_dns_config();                // 고정 IP에서는 Wi-Fi 시작 전에 적용

    const esp_timer_create_args_t ta = { .callback = reconn_timer_cb, .name = "reconn" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_reconn_timer), TAG, "reconn timer");

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ip evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL), TAG, "ip evt");

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_cfg.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;       // 전 채널 스캔 후
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;   // 가장 강한 AP 에 접속
    wc.sta.rm_enabled  = 1;                           // 802.11k
    wc.sta.btm_enabled = 1;                           // 802.11v (AP 주도 로밍)
    wc.sta.ft_enabled  = 1;                           // 802.11r (빠른 재인증)

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");  // 설정은 앱 NVS 로만 관리
    setup_prepare_wifi();   // 설정 모드 이름 결정, SoftAP 사용 시 AP 설정 등록 (평소에는 STA 전용)
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_country_code("KR", true));   // 채널 1~13
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    // 절전 해제 — 기본값(MIN_MODEM)은 하향 패킷이 DTIM 주기만큼(수백 ms) 지연됨
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "ps");
    // 혼잡한 2.4GHz 에서 20MHz 폭이 더 안정적
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_bandwidth(WIFI_IF_STA, BR_WIFI_BW20));
    // AP 비콘이 3초간 없으면 끊김 처리 (기본 6초) — 이동 중 AP 이탈을 빨리 감지해 재접속
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_inactive_time(WIFI_IF_STA, 3));

    ESP_LOGI(TAG, "connecting to \"%s\" ...", g_cfg.ssid);
    return esp_wifi_connect();
}

static esp_err_t sock_init(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(s_sock >= 0, ESP_FAIL, TAG, "socket");

    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));   // RFID 브로드캐스트용
    int tos = DSCP_EF_TOS;
    setsockopt(s_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    struct sockaddr_in a = {
        .sin_family      = AF_INET,
        .sin_port        = htons(g_cfg.local_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind %u failed errno=%d", g_cfg.local_port, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 서버 원격 설정 — 콘솔과 같은 명령 (cmd.c). save 는 확인 대기로 저장:
// 재부팅 후 서버 통신이 없으면 이전 설정으로 자동 복구 (잘못된 SSID/IP 로 연결이 끊기는 것 방지)
static void handle_cfg_cmd(const struct sockaddr_in *src, uint16_t seq, const uint8_t *pl, size_t len)
{
    static char line[CMD_LINE_MAX];     // net_rx 태스크 전용
    static char out[BR_PAYLOAD_MAX];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    cmd_act_t act;

    memcpy(line, pl, n);
    line[n] = '\0';
    size_t m = cmd_exec(line, CMD_SRC_SERVER, out, sizeof(out), &act);
    send_pkt(src->sin_addr.s_addr, ntohs(src->sin_port), BR_T_CFG_RESULT, seq, (const uint8_t *)out, m);
    if (act == CMD_ACT_REBOOT) {
        // 결과 패킷 송신 여유를 두고 설정 관리 태스크가 재부팅 (net_rx 를 막지 않음).
        // cmd_exec 확인 뒤에 메인보드 굽기가 시작됐으면 끝날 때까지 미룸
        setup_reboot(false);
    }
}

// 서버 → ESP 수신 처리
static void net_rx_task(void *arg)
{
    static uint8_t buf[BR_HDR_LEN + BR_PAYLOAD_MAX + 1];   // +1: 초과 길이 패킷 판별용
    struct sockaddr_in src;
    br_hdr_t h;
    const uint8_t *pl;

    for (;;) {
        socklen_t sl = sizeof(src);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!br_parse(buf, (size_t)n, &h, &pl)) {
            s_st.rx_bad++;
            continue;
        }
        if (src.sin_addr.s_addr != g_cfg.server_ip) {   // 설정된 서버 외 송신원 차단
            s_st.rx_foreign++;
            continue;
        }
        s_st.rx_pkts++;
        ota_confirm();      // 서버 하향 수신 = Wi-Fi·설정 정상 → 새 펌웨어 확정 (미확정 상태일 때만 동작)
        cfg_confirm();      // 원격 변경 설정도 같은 기준으로 확정

        switch (h.type) {
        case BR_T_MAIN_DOWN:
            if (h.agv_id != g_cfg.agv_id) {
                s_st.rx_foreign++;
                break;
            }
            uart_bridge_send_main(pl, h.len);   // 실패 시 uart tx_drop 카운트
            if (g_monitor) {
                ESP_LOG_BUFFER_HEXDUMP("MAIN<", pl, h.len, ESP_LOG_INFO);
            }
            break;
        case BR_T_PING:
            // seq·payload 그대로 반송 → 서버에서 왕복 지연 계산
            send_pkt(src.sin_addr.s_addr, ntohs(src.sin_port), BR_T_PONG, h.seq, pl, h.len);
            break;
        case BR_T_OTA_START: {
            if (h.agv_id != g_cfg.agv_id) {
                s_st.rx_foreign++;
                break;
            }
            char url[257];
            if (h.len >= sizeof(url)) {
                url[0] = '\0';                 // 잘라 쓰지 않음 → ota_start 가 bad_url 로 거부·보고
            } else {
                memcpy(url, pl, h.len);
                url[h.len] = '\0';
            }
            ota_start(url);                     // 거부·실패 사유는 ota_start 가 직접 보고 (OTA_RESULT)
            break;
        }
        case BR_T_CFG_CMD:
            if (h.agv_id != g_cfg.agv_id) {
                s_st.rx_foreign++;
                break;
            }
            handle_cfg_cmd(&src, h.seq, pl, h.len);
            break;
        case BR_T_INFO_REQ:
            send_info(&src);
            break;
        case BR_T_DSP_OTA_START:
            if (h.agv_id != g_cfg.agv_id) {
                s_st.rx_foreign++;
                break;
            }
            dsp_ota_start((const char *)pl, h.len);
            break;
        case BR_T_DSP_OTA_APPLY:
            if (h.agv_id != g_cfg.agv_id) {
                s_st.rx_foreign++;
                break;
            }
            dsp_ota_apply((const char *)pl, h.len);
            break;
        default:
            s_st.rx_bad++;
            break;
        }
    }
}

// 주기 상태 보고 (서버에서 RSSI/로밍/오류 추이 기록용)
static void hb_task(void *arg)
{
    char js[640];
    const char *fw   = esp_app_get_description()->version;
    const char *part = ota_running_partition();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);    // INFO 와 같은 STA MAC — 서버가 같은 호기 번호 충돌을 판정

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(g_cfg.hb_ms));
        if (!net_is_up()) {
            continue;
        }
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        const uart_stat_t *m = uart_bridge_stat(PORT_MAIN);
        const uart_stat_t *r = uart_bridge_stat(PORT_RFID);
        unsigned long u_err = m->overflow + m->rx_err + m->overlong + m->no_etx + m->tx_drop
                            + r->overflow + r->rx_err + r->overlong;

        int n = snprintf(js, sizeof(js),
                         "{\"fw\":\"%s\",\"mac\":\"" MACSTR "\",\"part\":\"%s\",\"ota\":\"%s\",\"up\":%lld,\"rssi\":%d,\"ch\":%u,\"bssid\":\"" MACSTR "\","
                         "\"disc\":%lu,\"reason\":%u,\"m_rx\":%lu,\"m_tx\":%lu,\"r_rx\":%lu,"
                         "\"u_err\":%lu,\"n_off\":%lu,\"n_err\":%lu,\"heap\":%lu,\"cfg\":\"%s\",\"setup\":%d,",
                         fw, MAC2STR(mac), part, ota_state_str(), (long long)(esp_timer_get_time() / 1000000), ap.rssi, ap.primary, MAC2STR(ap.bssid),
                         (unsigned long)s_st.disconnects, s_st.last_reason,
                         (unsigned long)m->rx_frames, (unsigned long)m->tx_frames, (unsigned long)r->rx_frames,
                         u_err, (unsigned long)s_st.tx_offline, (unsigned long)s_st.tx_err,
                         (unsigned long)esp_get_free_heap_size(), cfg_state_str(), setup_active());
        if (n > 0 && (size_t)n < sizeof(js)) {
            n += dsp_ota_status_json(js + n, sizeof(js) - n);
            n += snprintf(js + n, sizeof(js) - n, "}");
        }
        if (n > 0 && (size_t)n < sizeof(js)) {
            send_pkt(g_cfg.server_ip, g_cfg.server_port, BR_T_STATUS, s_seq_status++, (const uint8_t *)js, (size_t)n);
        }
    }
}

esp_err_t net_bridge_start(void)
{
    s_evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_evt, ESP_ERR_NO_MEM, TAG, "event group");
    // lwIP(tcpip 태스크)가 먼저 떠야 socket() 사용 가능
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_ERROR(sock_init(), TAG, "socket");
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "wifi");

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(net_rx_task, "net_rx", 5120, NULL, RX_TASK_PRIO, NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "net_rx task");
    if (g_cfg.hb_ms > 0) {
        ESP_RETURN_ON_FALSE(xTaskCreate(hb_task, "hb", 4096, NULL, HB_TASK_PRIO, NULL) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "hb task");
    }
    return ESP_OK;
}

size_t net_format_status(char *buf, size_t cap)
{
    esp_netif_ip_info_t ip = {0};
    wifi_ap_record_t ap = {0};
    uint8_t mac[6] = {0};
    bool up = net_is_up();

    if (cap == 0) {
        return 0;
    }
    esp_read_mac(mac, ESP_MAC_WIFI_STA);    // 이 브리지의 STA MAC (INFO·heartbeat 의 mac 과 같음)
    if (s_netif) {
        esp_netif_get_ip_info(s_netif, &ip);
    }
    if (up) {
        esp_wifi_sta_get_ap_info(&ap);
    }
    int n = snprintf(buf, cap,
                     "wifi: %s mac=" MACSTR " ip=" IPSTR " rssi=%d ch=%u bssid=" MACSTR "\n"
                     "net : tx=%lu tx_err=%lu tx_offline=%lu rx=%lu rx_bad=%lu rx_foreign=%lu disc=%lu last_reason=%u\n",
                     up ? "UP" : "DOWN", MAC2STR(mac), IP2STR(&ip.ip), ap.rssi, ap.primary, MAC2STR(ap.bssid),
                     (unsigned long)s_st.tx_pkts, (unsigned long)s_st.tx_err, (unsigned long)s_st.tx_offline,
                     (unsigned long)s_st.rx_pkts, (unsigned long)s_st.rx_bad, (unsigned long)s_st.rx_foreign,
                     (unsigned long)s_st.disconnects, s_st.last_reason);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

void net_print_status(void)
{
    char buf[320];
    net_format_status(buf, sizeof(buf));
    fputs(buf, stdout);
}
