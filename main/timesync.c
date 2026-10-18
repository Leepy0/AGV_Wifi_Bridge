#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "bridge_util.h"
#include "net_bridge.h"
#include "timesync.h"

static const char *TAG = "time";

#define RESYNC_MS       (600 * 1000)    // 맞춘 뒤 다시 맞추는 주기
#define RETRY_FAST_MS   2000            // 못 맞춘 동안의 요청 간격 (처음 FAST_TRIES 번)
#define RETRY_SLOW_MS   30000           // 그래도 답이 없으면 (구 서버·서버 시계 오류) 이 간격
#define FAST_TRIES      6

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // hb 태스크(요청) · net_rx(응답) · 명령 태스크(표시) 사이의 짧은 복사 보호
static int64_t  s_pending_t0 = -1;      // 응답을 기다리는 요청의 t0 (가동 ms)
static int64_t  s_next_ms;              // 다음 요청 시각 (가동 ms)
static int      s_tries;
static bool     s_synced;
static int64_t  s_sync_ms;              // 마지막으로 맞춘 가동 ms
static int32_t  s_rtt_ms = -1;
static int64_t  s_step_ms;              // 마지막 보정량 (새 시각 − 직전 브리지 시각). 처음 맞출 때는 0
static uint32_t s_count;

static int64_t up_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void timesync_on_wifi_up(void)
{
    portENTER_CRITICAL(&s_mux);
    s_next_ms = up_ms() + 500;      // 연결 직후 곧 묻는다 (ARP·서버 경로가 자리 잡을 시간만)
    s_tries = 0;
    portEXIT_CRITICAL(&s_mux);
}

void timesync_tick(void)
{
    int64_t now = up_ms();
    bool due;
    portENTER_CRITICAL(&s_mux);
    due = now >= s_next_ms;
    if (due) {
        s_pending_t0 = now;
        s_tries++;
        s_next_ms = now + (s_tries < FAST_TRIES ? RETRY_FAST_MS : RETRY_SLOW_MS);   // 응답이 오면 timesync_on_packet 이 RESYNC_MS 로 바꿈
    }
    portEXIT_CRITICAL(&s_mux);
    if (!due) {
        return;
    }
    char js[40];
    int n = snprintf(js, sizeof(js), "{\"t0\":%lld}", (long long)now);
    if (n > 0 && (size_t)n < sizeof(js)) {
        net_send_time_req((const uint8_t *)js, (size_t)n);
    }
}

bool timesync_on_packet(const uint8_t *pl, size_t len)
{
    bu_time_t t;
    int64_t up = up_ms();
    int64_t pending;
    portENTER_CRITICAL(&s_mux);
    pending = s_pending_t0;
    portEXIT_CRITICAL(&s_mux);
    if (!bu_time_solve((const char *)pl, len, up, pending, &t)) {
        ESP_LOGD(TAG, "TIME packet ignored (%u B)", (unsigned)len);
        return false;
    }
    bool was = false;
    int64_t before = wall_ms();
    portENTER_CRITICAL(&s_mux);
    was = s_synced;
    portEXIT_CRITICAL(&s_mux);
    struct timeval tv = { .tv_sec = (time_t)(t.unix_ms / 1000), .tv_usec = (suseconds_t)(t.unix_ms % 1000 * 1000) };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_synced = true;
    s_sync_ms = up;
    s_rtt_ms = t.rtt_ms;
    s_step_ms = was ? t.unix_ms - before : 0;
    s_count++;
    if (!t.pushed) {
        s_pending_t0 = -1;
    }
    s_tries = 0;
    s_next_ms = up + RESYNC_MS;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "synced from server (%s, rtt %d ms, step %lld ms)", t.pushed ? "push" : "reply", (int)t.rtt_ms, (long long)(was ? t.unix_ms - before : 0));
    return true;
}

bool timesync_now_ms(int64_t *unix_ms)
{
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = s_synced;
    portEXIT_CRITICAL(&s_mux);
    if (ok && unix_ms) {
        *unix_ms = wall_ms();
    }
    return ok;
}

int timesync_age_s(void)
{
    int64_t at;
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = s_synced;
    at = s_sync_ms;
    portEXIT_CRITICAL(&s_mux);
    return ok ? (int)((up_ms() - at) / 1000) : -1;
}

static size_t clamp(int n, size_t cap)
{
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : (cap ? cap - 1 : 0));
}

size_t timesync_hb_fields(char *buf, size_t cap)
{
    int64_t ms;
    if (timesync_now_ms(&ms)) {
        return clamp(snprintf(buf, cap, "\"tsync\":%d,\"ts\":%lld,", timesync_age_s(), (long long)ms), cap);
    }
    return clamp(snprintf(buf, cap, "\"tsync\":-1,"), cap);
}

size_t timesync_format(char *buf, size_t cap)
{
    int64_t ms;
    int32_t rtt;
    int64_t step;
    uint32_t count;
    int tries;
    portENTER_CRITICAL(&s_mux);
    rtt = s_rtt_ms; step = s_step_ms; count = s_count; tries = s_tries;
    portEXIT_CRITICAL(&s_mux);
    if (!timesync_now_ms(&ms)) {
        return clamp(snprintf(buf, cap, "time: not synced (asking the server, %d tries)\n", tries), cap);
    }
    time_t sec = (time_t)(ms / 1000);
    struct tm tm;
    char iso[24];
    gmtime_r(&sec, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return clamp(snprintf(buf, cap, "time: %s synced %d s ago (rtt %d ms, last step %+lld ms, %lu syncs)\n",
                          iso, timesync_age_s(), (int)rtt, (long long)step, (unsigned long)count), cap);
}

size_t timesync_json(char *buf, size_t cap)
{
    int64_t ms;
    int32_t rtt;
    int64_t step;
    uint32_t count;
    portENTER_CRITICAL(&s_mux);
    rtt = s_rtt_ms; step = s_step_ms; count = s_count;
    portEXIT_CRITICAL(&s_mux);
    int n;
    if (timesync_now_ms(&ms)) {
        n = snprintf(buf, cap, "{\"sync\":true,\"unix\":%lld,\"age\":%d,\"rtt\":%d,\"step\":%lld,\"count\":%lu}",
                     (long long)ms, timesync_age_s(), (int)rtt, (long long)step, (unsigned long)count);
    } else {
        n = snprintf(buf, cap, "{\"sync\":false,\"unix\":0,\"age\":-1}");
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}
