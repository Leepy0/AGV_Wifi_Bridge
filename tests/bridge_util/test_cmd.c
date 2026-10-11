// main/cmd.c 호스트 시험: 메인보드 프로그래밍 중 원격 reboot 거부(B-05), 원격 save 안내(B-02), hb_ms 도움말(C-06)
#include <string.h>
#include "app_config.h"
#include "net_bridge.h"
#include "ota.h"
#include "setup.h"
#include "dsp_ota.h"
#include "cmd.h"
#include "test_common.h"

app_cfg_t g_cfg;
volatile bool g_monitor;
static bool s_dsp_prog;
static int s_saves;
static bool s_last_trial;

bool dsp_ota_busy(void) { return s_dsp_prog; }
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

    DONE("test_cmd");
}
