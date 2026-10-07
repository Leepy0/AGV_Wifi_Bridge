#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "uart_bridge.h"
#include "net_bridge.h"
#include "ota.h"
#include "setup_ap.h"
#include "esp_app_desc.h"
#include "console_cmd.h"

static const char *TAG = "console";

static int cmd_cfg(int argc, char **argv)
{
    cfg_print();
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: set <key> <value>\n"
               "keys : agv ssid pass(- = none) ip(dhcp) mask gw server server_port local_port\n"
               "       rfid_dst(off / 255.255.255.255) rfid_port main_baud rfid_baud hb_ms(0 = off)\n");
        return 1;
    }
    if (!cfg_set(argv[1], argv[2])) {
        printf("invalid key or value\n");
        return 1;
    }
    printf("ok - 'save' then 'reboot' to apply\n");
    return 0;
}

static int cmd_save(int argc, char **argv)
{
    esp_err_t err = cfg_save(false);
    if (err != ESP_OK) {
        printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved\n");
    return 0;
}

static int cmd_defaults(int argc, char **argv)
{
    esp_err_t err = cfg_erase();
    if (err != ESP_OK) {
        printf("erase failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved config erased - reboot to use build defaults\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static void print_uart(const char *name, const uart_stat_t *s)
{
    printf("%s: rx_frm=%lu rx_B=%lu tx_frm=%lu tx_B=%lu ovf=%lu err=%lu long=%lu no_etx=%lu resync=%lu tx_drop=%lu\n", name,
           (unsigned long)s->rx_frames, (unsigned long)s->rx_bytes,
           (unsigned long)s->tx_frames, (unsigned long)s->tx_bytes,
           (unsigned long)s->overflow, (unsigned long)s->rx_err,
           (unsigned long)s->overlong, (unsigned long)s->no_etx,
           (unsigned long)s->resync, (unsigned long)s->tx_drop);
}

static int cmd_stat(int argc, char **argv)
{
    char ap[96];
    net_print_status();
    setup_ap_format(ap, sizeof(ap));
    fputs(ap, stdout);
    print_uart("main", uart_bridge_stat(PORT_MAIN));
    print_uart("rfid", uart_bridge_stat(PORT_RFID));
    printf("heap: %lu\n", (unsigned long)esp_get_free_heap_size());
    return 0;
}

// ota            : 상태 표시
// ota <url>      : 벤치용 수동 업데이트 (현장은 서버가 OTA_START 패킷으로 지시)
// ota confirm    : 롤백 대기 중인 펌웨어를 수동 확정
static int cmd_ota(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "confirm") == 0) {
        ota_confirm();
    } else if (argc == 2) {
        esp_err_t err = ota_start(argv[1]);
        if (err != ESP_OK) {
            printf("ota start failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("ota started - progress is sent to server, reboot on success\n");
        return 0;
    }
    printf("fw=%s part=%s state=%s\n", esp_app_get_description()->version, ota_running_partition(), ota_state_str());
    return 0;
}

// setup        : 무선 설정 AP 상태
// setup on|off : 설정 AP 열기/닫기 (휴대폰으로 AP 접속 → http://192.168.4.1)
static int cmd_setup(int argc, char **argv)
{
    char buf[96];
    if (argc == 2) {
        setup_ap_request(strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0);
    }
    setup_ap_format(buf, sizeof(buf));
    fputs(buf, stdout);
    return 0;
}

static int cmd_mon(int argc, char **argv)
{
    if (argc == 2) {
        g_monitor = (strcmp(argv[1], "1") == 0 || strcmp(argv[1], "on") == 0);
    }
    printf("monitor %s\n", g_monitor ? "on" : "off");
    return 0;
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "bridge>";
    rc.max_cmdline_length = 160;

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw, &rc, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl);
#endif
    if (err != ESP_OK) {
        // 콘솔 실패는 브리지 동작과 무관하므로 로그만 남김
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return;
    }

    const esp_console_cmd_t cmds[] = {
        { .command = "cfg",      .help = "show config",                         .func = cmd_cfg },
        { .command = "set",      .help = "set <key> <value> (run without args for keys)", .func = cmd_set },
        { .command = "save",     .help = "save config to NVS",                  .func = cmd_save },
        { .command = "defaults", .help = "erase saved config (build defaults on reboot)", .func = cmd_defaults },
        { .command = "reboot",   .help = "restart",                             .func = cmd_reboot },
        { .command = "stat",     .help = "wifi / network / uart counters",      .func = cmd_stat },
        { .command = "mon",      .help = "mon <0|1> : hex dump every frame",    .func = cmd_mon },
        { .command = "ota",      .help = "ota [<url>|confirm] : firmware update / confirm", .func = cmd_ota },
        { .command = "setup",    .help = "setup [on|off] : wireless setup AP (http://192.168.4.1)", .func = cmd_setup },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_start_repl(repl));
}
