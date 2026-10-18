// main/cmd.c 호스트 시험: 메인보드 프로그래밍 중 원격 reboot 거부(B-05), 원격 save 안내(B-02), hb_ms 도움말(C-06)
#include <string.h>
#include "esp_mac.h"
#include "app_config.h"
#include "net_bridge.h"
#include "ota.h"
#include "setup.h"
#include "dsp_ota.h"
#include "timesync.h"
#include "cmd.h"
#include "test_common.h"

app_cfg_t g_cfg;
volatile bool g_monitor;
static bool s_dsp_prog;
static int s_saves;
static bool s_last_trial;

static int s_time_lines;
size_t timesync_format(char *buf, size_t cap) { s_time_lines++; return (size_t)snprintf(buf, cap, "time: not synced (asking the server, 0 tries)\n"); }
static bool s_dsp_stage;
static char s_test_host[40];
static int s_scans, s_tests;
bool dsp_ota_busy(void) { return s_dsp_prog; }
bool dsp_ota_staging(void) { return s_dsp_stage; }
size_t net_scan_json(char *buf, size_t cap) { s_scans++; return (size_t)snprintf(buf, cap, "[{\"ssid\":\"AGV_WIFI\",\"rssi\":-48,\"ch\":6,\"auth\":\"wpa2\"}]"); }
size_t net_test(const char *host, char *buf, size_t cap) {
    s_tests++;
    snprintf(s_test_host, sizeof(s_test_host), "%s", host ? host : "(server)");
    return (size_t)snprintf(buf, cap, "ping x: 3/3 ok, avg 4 ms\n");
}
size_t dsp_ota_format(char *buf, size_t cap) { buf[0] = '\0'; return 0; }
void dsp_ota_query_version(void) {}
void dsp_ota_apply(const char *json, size_t len) {}
size_t cfg_format(char *buf, size_t cap) { buf[0] = '\0'; return 0; }
bool cfg_set(const char *key, const char *val) { return true; }
esp_err_t cfg_save(bool trial) { s_saves++; s_last_trial = trial; return ESP_OK; }
esp_err_t cfg_erase(void) { return ESP_OK; }
const char *cfg_state_str(void) { return "saved"; }
size_t net_format_status(char *buf, size_t cap) { buf[0] = '\0'; return 0; }
size_t setup_format(char *buf, size_t cap) { buf[0] = '\0'; return 0; }
void setup_request(bool open) {}
const char *ota_running_partition(void) { return "ota_0"; }
const char *ota_state_str(void) { return "valid"; }
static esp_app_desc_t s_desc = { "0.5.2", "agv_wifi_bridge" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_desc; }
const char *esp_err_to_name(esp_err_t e) { return "ESP_ERR"; }
static int s_discards;
void cfg_discard(void) { s_discards++; }
size_t cfg_format_json(char *buf, size_t cap) {
    return (size_t)snprintf(buf, cap, "\"state\":\"saved\",\"differs\":false,\"cfg\":{\"ssid\":\"A\\\"B\"},\"keys\":[\"agv\"]}");
}
size_t net_format_status_json(char *buf, size_t cap) {
    return (size_t)snprintf(buf, cap, "{\"fw\":\"0.5.5\",\"wifi\":{\"up\":true},\"heap\":1}");
}
const char *setup_name(void) { return "AGVBR-07-E5F6"; }
const char *esp_get_idf_version(void) { return "v6.1"; }
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    static const uint8_t m[6] = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6 };
    memcpy(mac, m, sizeof(m));
    return ESP_OK;
}

// 한 줄이고 중괄호·대괄호가 맞고 문자열이 닫힌 JSON 인지만 확인 (호스트 시험용 간이 검사)
static bool json_balanced(const char *s)
{
    int depth = 0;
    bool str = false;
    for (; *s; s++) {
        if (str) {
            if (*s == '\\') {
                if (*++s == '\0') return false;
            } else if (*s == '"') {
                str = false;
            }
        } else if (*s == '"') {
            str = true;
        } else if (*s == '{' || *s == '[') {
            depth++;
        } else if (*s == '}' || *s == ']') {
            if (--depth < 0) return false;
        } else if (*s == '\n') {
            return false;
        }
    }
    return depth == 0 && !str;
}

static size_t run(const char *cmd, cmd_src_t src, char *out, size_t cap, cmd_act_t *act)
{
    char line[200];
    strcpy(line, cmd);
    return cmd_exec(line, src, out, cap, act);
}

int main(void)
{
    char out[600];
    cmd_act_t act;

    // B-05: 프로그래밍 중이면 원격·BLE reboot 거부
    s_dsp_prog = true;
    run("reboot", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(act == CMD_ACT_NONE);
    CHECK(strstr(out, "busy: mainboard programming") != NULL);
    run("reboot", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(act == CMD_ACT_NONE);
    s_dsp_prog = false;
    run("reboot", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(act == CMD_ACT_REBOOT);

    // B-02: 원격 save = trial, 180초 안내
    run("save", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(s_saves == 1 && s_last_trial);
    CHECK(strstr(out, "180 s") != NULL && strstr(out, "0.3.0") != NULL);
    run("save", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(s_saves == 2 && !s_last_trial);
    CHECK(strstr(out, "180") == NULL);

    // C-06: 도움말 범위
    run("set", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(strstr(out, "hb_ms(0 = off, 100-2000)") != NULL);

    // BR-1: cfg json · stat json — 한 줄, 식별 필드(name·mac·idf)와 cfg 본문이 함께 들어감
    run("cfg json", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(json_balanced(out) && out[0] == '{');
    CHECK(strstr(out, "\"name\":\"AGVBR-07-E5F6\"") != NULL && strstr(out, "\"idf\":\"v6.1\"") != NULL);
    CHECK(strstr(out, "\"mac\":\"a1:b2:c3:d4:e5:f6\"") != NULL && strstr(out, "\"keys\":[\"agv\"]}") != NULL);
    // 자리가 모자라면 잘린 JSON 대신 오류 객체
    run("cfg json", CMD_SRC_LOCAL, out, 60, &act);
    CHECK(strcmp(out, "{\"err\":\"overflow\"}") == 0);
    run("stat json", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(json_balanced(out) && strcmp(out, "{\"fw\":\"0.5.5\",\"wifi\":{\"up\":true},\"heap\":1}") == 0);
    run("stat", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(strstr(out, "fw=0.5.2") != NULL);     // 인자 없는 stat 은 기존 텍스트 그대로

    // BR-3: discard — 편집 사본 되돌림, 모든 출처에서 가능
    run("discard", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(s_discards == 1 && act == CMD_ACT_NONE);
    CHECK(strstr(out, "edit copy now equals running config") != NULL);
    run("discard", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(s_discards == 2);
    run("help", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(strstr(out, "discard") != NULL && strstr(out, "cfg [json]") != NULL);

    // BR-4/5: scan · nettest — 평소엔 그대로 전달, 메인보드 굽기·스테이징 중에는 거부
    run("scan", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(s_scans == 1 && json_balanced(out) && out[0] == '[' && strstr(out, "\"auth\":\"wpa2\"") != NULL);
    run("nettest", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(s_tests == 1 && strcmp(s_test_host, "(server)") == 0 && strstr(out, "3/3 ok") != NULL);
    run("nettest gateway.local", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(s_tests == 2 && strcmp(s_test_host, "gateway.local") == 0);
    s_dsp_stage = true;
    run("scan", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(s_scans == 1 && strstr(out, "busy:") != NULL);
    run("nettest", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(s_tests == 2 && strstr(out, "busy:") != NULL);
    s_dsp_stage = false;
    s_dsp_prog = true;
    run("scan", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(s_scans == 1 && strstr(out, "busy:") != NULL);
    s_dsp_prog = false;
    run("help", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(strstr(out, "scan") != NULL && strstr(out, "nettest [host]") != NULL);

    // 시각 동기: time 명령, stat 텍스트에도 한 줄
    run("time", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(strncmp(out, "time: ", 6) == 0);
    int before = s_time_lines;
    run("stat", CMD_SRC_SERVER, out, sizeof(out), &act);
    CHECK(s_time_lines == before + 1 && strstr(out, "time: not synced") != NULL);
    run("help", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(strstr(out, "| time |") != NULL);

    // 빈 줄은 cfg 텍스트 (strtok 결과 NULL 경로)
    run("", CMD_SRC_LOCAL, out, sizeof(out), &act);
    CHECK(act == CMD_ACT_NONE);

    DONE("test_cmd");
}
