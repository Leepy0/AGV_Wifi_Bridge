#include <string.h>
#include <stdio.h>
#include "esp_app_desc.h"
#include "app_config.h"
#include "net_bridge.h"
#include "ota.h"
#include "setup.h"
#include "dsp_ota.h"
#include "cmd.h"

static size_t clamp(int n, size_t cap)
{
    if (n < 0) {
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t cmd_exec(char *line, cmd_src_t src, char *out, size_t cap, cmd_act_t *act)
{
    size_t n;

    *act = CMD_ACT_NONE;
    line[strcspn(line, "\r\n")] = '\0';
    for (n = strlen(line); n > 0 && line[n - 1] == ' '; n--) {
        line[n - 1] = '\0';
    }

    char *rest = NULL;
    char *cmd  = strtok_r(line, " ", &rest);
    if (cmd == NULL || strcmp(cmd, "cfg") == 0) {
        return cfg_format(out, cap);
    }
    if (strcmp(cmd, "set") == 0) {
        char *key = strtok_r(NULL, " ", &rest);
        char *val = rest ? rest + strspn(rest, " ") : NULL;    // 값은 줄 끝까지 (공백 있는 SSID 허용)
        if (key == NULL || val == NULL || *val == '\0') {
            return clamp(snprintf(out, cap, "usage: set <key> <value>\n"
                                  "keys : agv ssid pass(- = none) ip(dhcp) mask gw server server_port local_port\n"
                                  "       rfid_dst(off / 255.255.255.255) rfid_port main_baud rfid_baud hb_ms(0 = off, 100-2000)\n"), cap);
        }
        if (cfg_set(key, val)) {
            return clamp(snprintf(out, cap, "ok %s - 'save' then 'reboot' to apply\n", key), cap);
        }
        return clamp(snprintf(out, cap, "invalid key or value: %s\n", key), cap);
    }
    if (strcmp(cmd, "save") == 0) {
        esp_err_t err = cfg_save(src == CMD_SRC_SERVER);
        if (err != ESP_OK) {
            return clamp(snprintf(out, cap, "save failed: %s\n", esp_err_to_name(err)), cap);
        }
        return clamp(snprintf(out, cap, src == CMD_SRC_SERVER
                              ? "saved (trial) - 'reboot' to apply. After reboot the bridge needs a server packet within 180 s "
                                "or it restores the previous config (gateway 0.3.0+ pings automatically)\n"
                              : "saved - 'reboot' to apply\n"), cap);
    }
    if (strcmp(cmd, "defaults") == 0 && src == CMD_SRC_LOCAL) {
        esp_err_t err = cfg_erase();
        return clamp(err == ESP_OK
                     ? snprintf(out, cap, "saved config erased - 'reboot' to use build defaults\n")
                     : snprintf(out, cap, "erase failed: %s\n", esp_err_to_name(err)), cap);
    }
    if (strcmp(cmd, "reboot") == 0) {
        if (dsp_ota_busy()) {
            // 메인보드를 지운 뒤 재부팅하면 앱 없이 부트로더에 남는다 — 굽기가 끝난 뒤 다시
            return clamp(snprintf(out, cap, "busy: mainboard programming - reboot refused, retry when done\n"), cap);
        }
        *act = CMD_ACT_REBOOT;
        return clamp(snprintf(out, cap, "rebooting\n"), cap);
    }
    if (strcmp(cmd, "stat") == 0) {
        n = clamp(snprintf(out, cap, "fw=%s part=%s ota=%s cfg=%s\n", esp_app_get_description()->version,
                           ota_running_partition(), ota_state_str(), cfg_state_str()), cap);
        n += net_format_status(out + n, cap - n);
        n += setup_format(out + n, cap - n);
        n += dsp_ota_format(out + n, cap - n);
        return n;
    }
    if (strcmp(cmd, "setup") == 0) {
        char *arg = strtok_r(NULL, " ", &rest);
        if (arg) {
            setup_request(strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
        }
        return setup_format(out, cap);
    }
    if (strcmp(cmd, "dsp") == 0) {
        // dsp | dsp ver | dsp apply   (스테이징은 서버 패킷으로만)
        char *arg = strtok_r(NULL, " ", &rest);
        if (arg && strcmp(arg, "ver") == 0) {
            dsp_ota_query_version();
            return clamp(snprintf(out, cap, "version query sent - 'dsp' to see\n"), cap);
        }
        if (arg && strcmp(arg, "apply") == 0) {
            dsp_ota_apply("{}", 2);
            return clamp(snprintf(out, cap, "apply requested - progress goes to server (DSP_OTA_RESULT)\n"), cap);
        }
        return dsp_ota_format(out, cap);
    }
    if (strcmp(cmd, "help") == 0) {
        return clamp(snprintf(out, cap, "cfg | set <key> <value> | save | reboot | stat | setup [on|off] | dsp [ver|apply]%s\n",
                              src == CMD_SRC_LOCAL ? " | defaults" : ""), cap);
    }
    return clamp(snprintf(out, cap, "unknown command '%s' - type help\n", cmd), cap);
}
