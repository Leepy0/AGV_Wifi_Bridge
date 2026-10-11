// main/ota.c 호스트 시험: URL 호스트 제한(X-08), 메인보드 작업 중 거부(B-05), 최종 결과 3회 송신
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "net_bridge.h"
#include "dsp_ota.h"
#include "ota.h"
#include "test_common.h"

// ───── 스텁 ─────
app_cfg_t g_cfg;
volatile bool g_monitor;

#define PKT_MAX 16
static char   s_pkt[PKT_MAX][512];
static int    s_npkt;
bool net_send_ota_result(const uint8_t *data, size_t len)
{
    if (s_npkt < PKT_MAX) {
        memcpy(s_pkt[s_npkt], data, len);
        s_pkt[s_npkt][len] = '\0';
        s_npkt++;
    }
    return true;
}

static bool s_dsp_prog;
static const char *s_dsp_st = "idle";
bool dsp_ota_busy(void) { return s_dsp_prog; }
bool dsp_ota_staging(void) { return strcmp(s_dsp_st, "downloading") == 0; }

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
static TaskFunction_t s_task_fn;
static void *s_task_arg;
static int s_tasks;
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t st, void *arg, UBaseType_t p, TaskHandle_t *h)
{
    s_task_fn = f;
    s_task_arg = arg;
    s_tasks++;
    return pdPASS;
}

static esp_timer_cb_t s_tcb;
static void *s_targ;
static bool s_tarmed;
static int s_timers;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    s_tcb = a->callback;
    s_targ = a->arg;
    s_timers++;
    *h = (void *)&s_tcb;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us)
{
    if (s_tarmed) return ESP_ERR_INVALID_STATE;
    s_tarmed = true;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h)
{
    if (!s_tarmed) return ESP_ERR_INVALID_STATE;
    s_tarmed = false;
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return 0; }
static int s_restarts;
void esp_restart(void) { s_restarts++; }

static esp_app_desc_t s_desc = { "0.5.2", "agv_wifi_bridge" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_desc; }
static esp_partition_t s_part = { "ota_0" };
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_part; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *st) { *st = ESP_OTA_IMG_VALID; return ESP_OK; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) { return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }

static esp_err_t s_begin_err;
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c, esp_https_ota_handle_t *h) { *h = (void *)1; return s_begin_err; }
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t h, esp_app_desc_t *d) { *d = (esp_app_desc_t){ "0.5.3", "agv_wifi_bridge" }; return ESP_OK; }
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h) { return ESP_OK; }
int esp_https_ota_get_image_size(esp_https_ota_handle_t h) { return 1000; }
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t h) { return 1000; }
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h) { return ESP_OK; }
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h) { return true; }
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h) { return ESP_OK; }

// ───── 도우미 ─────
static void reset(void)
{
    s_npkt = 0;
    s_tarmed = false;
    s_tasks = 0;
    s_dsp_prog = false;
    s_dsp_st = "idle";
}

// 재송신 타이머를 다 돌림 → 돌린 횟수
static int drain_timer(void)
{
    int n = 0;
    while (s_tarmed && n < 10) {
        s_tarmed = false;
        s_tcb(s_targ);
        n++;
    }
    return n;
}

// 마지막 결과가 같은 내용으로 정확히 3회 나갔는지
static bool sent3(const char *js)
{
    drain_timer();
    if (s_npkt < 3) return false;
    int same = 0;
    for (int i = 0; i < s_npkt; i++) {
        same += strcmp(s_pkt[i], js) == 0;
    }
    return same == 3 && strcmp(s_pkt[s_npkt - 1], js) == 0;
}

int main(void)
{
    uint8_t srv[4] = { 192, 168, 0, 10 };
    memcpy(&g_cfg.server_ip, srv, 4);
    ota_init();
    CHECK(s_timers == 1);           // 재송신 타이머 (확정 대기 아님)

    // 형식 오류
    reset();
    CHECK(ota_start("ftp://192.168.0.10/a.bin") == ESP_ERR_INVALID_ARG);
    CHECK(sent3("{\"stage\":\"fail\",\"err\":\"bad_url\"}"));
    CHECK(s_npkt == 3);

    // 결정 11: 서버가 아닌 호스트 거부 (출발지 위조 패킷의 임의 URL)
    reset();
    CHECK(ota_start("http://10.0.0.5/evil.bin") == ESP_ERR_INVALID_ARG);
    CHECK(sent3("{\"stage\":\"fail\",\"err\":\"url_host\"}"));
    CHECK(s_tasks == 0);
    reset();
    CHECK(ota_start("http://192.168.0.10@10.0.0.5/evil.bin") == ESP_ERR_INVALID_ARG);
    CHECK(s_tasks == 0);

    // B-05: 메인보드 프로그래밍·스테이징 중 거부, busy 표시는 남지 않음
    reset();
    s_dsp_prog = true;
    CHECK(ota_start("http://192.168.0.10:8080/fw.bin") == ESP_ERR_INVALID_STATE);
    CHECK(sent3("{\"stage\":\"fail\",\"err\":\"mainboard_programming\"}"));
    CHECK(!ota_busy() && s_tasks == 0);
    reset();
    s_dsp_st = "downloading";
    CHECK(ota_start("http://192.168.0.10:8080/fw.bin") == ESP_ERR_INVALID_STATE);
    CHECK(sent3("{\"stage\":\"fail\",\"err\":\"mainboard_staging\"}"));
    CHECK(!ota_busy() && s_tasks == 0);
    reset();
    s_dsp_st = "staged";            // 스테이징 완료 상태는 막지 않음
    CHECK(ota_start("http://192.168.0.10:8080/fw.bin") == ESP_OK);
    CHECK(ota_busy() && s_tasks == 1);
    CHECK(strcmp(ota_state_str(), "busy") == 0);

    // 진행 중 두 번째 요청 → busy
    s_npkt = 0;
    CHECK(ota_start("http://192.168.0.10:8080/fw.bin") == ESP_ERR_INVALID_STATE);
    CHECK(sent3("{\"stage\":\"busy\"}"));

    // 성공 경로: done 3회 후 재부팅
    s_npkt = 0;
    s_tarmed = false;
    s_task_fn(s_task_arg);
    CHECK(s_npkt >= 1 && strncmp(s_pkt[0], "{\"stage\":\"start\",\"url\":\"http://192.168.0.10:8080/fw.bin\"}", 60) == 0);
    CHECK(s_restarts == 1);
    CHECK(sent3("{\"stage\":\"done\",\"ver\":\"0.5.3\",\"bytes\":1000}"));
    CHECK(!ota_busy());             // (스텁 esp_restart 가 돌아오므로 정리 경로까지 감)

    // 실패 경로: fail 3회, busy 해제
    reset();
    s_begin_err = ESP_FAIL;
    CHECK(ota_start("http://192.168.0.10/fw.bin") == ESP_OK);
    s_npkt = 0;
    s_task_fn(s_task_arg);
    CHECK(sent3("{\"stage\":\"fail\",\"err\":\"begin ESP_ERR\"}"));
    CHECK(!ota_busy());

    // 재송신 중 새 최종 결과 → 새 결과로 대체 (옛 결과는 더 안 나감)
    reset();
    ota_start("ftp://x/a.bin");
    ota_start("http://10.0.0.5/a.bin");
    drain_timer();
    CHECK(s_npkt == 4);
    CHECK(strcmp(s_pkt[0], "{\"stage\":\"fail\",\"err\":\"bad_url\"}") == 0);
    CHECK(strcmp(s_pkt[1], "{\"stage\":\"fail\",\"err\":\"url_host\"}") == 0);
    CHECK(strcmp(s_pkt[3], "{\"stage\":\"fail\",\"err\":\"url_host\"}") == 0);

    DONE("test_ota_start");
}
