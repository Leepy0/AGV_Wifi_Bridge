#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "app_config.h"
#include "uart_bridge.h"
#include "net_bridge.h"
#include "ota.h"
#include "dsp_image.h"
#include "bootproto.h"
#include "dsp_app_proto.h"
#include "dsp_ota.h"

static const char *TAG = "dsp_ota";

#define PART_LABEL          "dspfw"
#define STAGE_HDR_LEN       64
#define STAGE_MAGIC         "DSPSTG1"
#define FILE_HDR_LEN        32
#define FILE_MAGIC          "AGVDSP1"
#define HTTP_BUF            4096
#define TASK_STACK          8192
#define TASK_PRIO           2           // 중계 태스크보다 낮게
#define APP_REPLY_MS        2000        // 앱 F→R 응답 대기
#define BOOT_START_MS       400         // UOK 후 워치독 리셋 → 부트로더 기동 여유
#define RESP_MS             800         // 부트로더 일반 응답
#define HELLO_MS            1500        // 첫 HELLO (앱 리셋 → 부트로더 기동 직후, 3회 재시도 = 4.5초)
#define ERASE_MS            20000
#define MAX_REDIRECTS       3
#define RETRIES             3
#define APP_UP_TRIES        8           // RESET 후 앱 기동 확인 (1초 간격)
#define VER_QUERY_RETRY_S   60          // 버전 모를 때 재조회 간격
#define PROG_RETRIES        2           // ERASE 이후 실패 시 HELLO 부터 다시 하는 횟수
#define WAIT_POWER_S        60          // 복구 적용: 전원 재투입을 기다리며 HELLO 를 계속 보내는 시간
#define WAIT_POWER_REPORT_S 10          // 그동안 wait_power 보고 간격 (서버 대기 시간 연장)
#define WAIT_POWER_HELLO_MS 50          // 부트로더는 리셋 직후 약 200ms 동안 HELLO 를 기다림
#define FINAL_REPORTS       3           // staged·done·fail 반복 횟수 (UDP 유실 대비, 서버는 중복에 안전)
#define FINAL_REPORT_GAP_MS 300
#define PEEK_LEN            (FILE_HDR_LEN + DSPIMG_HDR_WORDS * 2)   // 다운로드 머리 검사 (파일 헤더 + 앱 헤더)
#define RECENT_S            600         // 같은 job 의 중복 START·APPLY(서버 재전송)에 최근 결과를 다시 보고하는 기간

// 스테이징 파티션 머리 (64 B): 다운로드 완료 후 마지막에 기록 → 있으면 유효한 이미지
typedef struct {
    char     magic[8];
    uint32_t job;
    uint32_t file_len;      // .dspfw 전체 길이 (파티션 오프셋 64 부터)
    uint32_t file_crc;      // .dspfw 파일 헤더의 CRC (전체 무결성)
    uint32_t app_crc;       // 앱 헤더의 app_crc32
    char     ver[16];
    uint8_t  rsvd[24];
} stage_hdr_t;
_Static_assert(sizeof(stage_hdr_t) == STAGE_HDR_LEN, "stage header size");

typedef enum { CMD_QUERY_VER, CMD_START, CMD_APPLY } cmd_type_t;
typedef struct {
    cmd_type_t type;
    uint32_t   job;
    bool       apply;
    char       ver[16];
    uint32_t   crc32;       // 서버가 준 파일 CRC (0 = 미지정)
    uint32_t   size;        // 서버가 준 파일 크기 (0 = 미지정)
    bool       recover;     // 복구 적용: 앱이 U 에 응답하지 않아도 부트로더로 직행, HELLO 실패 시 전원 재투입 대기
    char       url[200];    // 199자 초과는 url_too_long 으로 거부 (잘라 쓰지 않음)
} cmd_t;

typedef enum { ST_IDLE, ST_DOWNLOADING, ST_STAGED, ST_PROGRAMMING } state_t;
static const char *ST_NAME[] = { "idle", "downloading", "staged", "programming" };

static const esp_partition_t *s_part;
static QueueHandle_t    s_q;
static volatile state_t s_state;
static stage_hdr_t      s_stage;            // 유효하면 magic 이 맞음
static char             s_dsp_ver[12] = "?";
static char             s_dsp_boot[12] = "?";
static char             s_dsp_build[12] = "?";
static char             s_pan = '1';        // E 이벤트에서 학습
static char             s_msg = '1';        // 브리지 MSG ID 카운터 1~F
static int64_t          s_last_query_us;
// 앱 응답 대기
static SemaphoreHandle_t s_reply_sem;
static volatile char     s_wait_msg;        // 기다리는 MSG ID (0 = 없음)
static dap_reply_t       s_reply;
static uint8_t           s_http[HTTP_BUF];
// 서버 재전송 대비 (같은 job 의 중복 명령). 처리 중인 job 은 dsp 태스크만 쓰고 서버 수신 태스크는 읽기만 한다
static volatile uint32_t s_cur_start;       // 처리 중인 START 의 job (apply:1 이면 이어지는 적용까지), 0 = 없음
static volatile uint32_t s_cur_apply;       // 적용 중인 job, 0 = 없음
// 최근 최종 결과 1건 — dsp 태스크만 읽고 쓴다 (경쟁 없음)
static struct {
    uint32_t job;               // 0 = 없음
    bool     applied;           // 적용 단계 결과 (done·fail) — false 면 스테이징 단계 결과 (staged·다운로드 fail)
    const char *stage;          // "staged" / "done" / "fail" (문자열 상수)
    int      pct;
    char     ver[16];           // "" = 없음
    char     err[24];           // "" = 없음
    int64_t  at_us;
} s_recent;

// ───────── 보고 ─────────

static void report(uint32_t job, const char *stage, int pct, const char *ver, const char *err)
{
    char js[256];
    char ev[100];                           // ver 는 메인보드·이미지에서 온 문자열 → 이스케이프
    int n = snprintf(js, sizeof(js), "{\"job\":%lu,\"stage\":\"%s\"", (unsigned long)job, stage);
    if (pct >= 0)  n += snprintf(js + n, sizeof(js) - n, ",\"pct\":%d", pct);
    if (ver) {
        dap_json_escape(ev, sizeof(ev), ver);
        n += snprintf(js + n, sizeof(js) - n, ",\"ver\":\"%s\"", ev);
    }
    if (err)       n += snprintf(js + n, sizeof(js) - n, ",\"err\":\"%s\"", err);
    n += snprintf(js + n, sizeof(js) - n, "}");
    if (n > 0 && (size_t)n < sizeof(js)) {
        net_send_dsp_result((const uint8_t *)js, (size_t)n);
    }
    ESP_LOGI(TAG, "job %lu %s%s%s", (unsigned long)job, stage, err ? " err=" : "", err ? err : "");
}

// 최종 보고(staged·done·fail): 같은 내용을 FINAL_REPORTS 회. gap = false 면 간격 없이 (서버 수신 태스크 문맥)
static void report_final_ex(uint32_t job, const char *stage, int pct, const char *ver, const char *err, bool gap)
{
    for (int i = 0; i < FINAL_REPORTS; i++) {
        if (i > 0 && gap) {
            vTaskDelay(pdMS_TO_TICKS(FINAL_REPORT_GAP_MS));
        }
        report(job, stage, pct, ver, err);
    }
}

static void report_final(uint32_t job, const char *stage, int pct, const char *ver, const char *err)
{
    report_final_ex(job, stage, pct, ver, err, true);
}

// dsp 태스크의 최종 보고: 보내고 최근 결과로 기억 (중복 명령에 다시 보고하려고). applied = 적용 단계 결과
static void report_result(uint32_t job, bool applied, const char *stage, int pct, const char *ver, const char *err)
{
    report_final(job, stage, pct, ver, err);
    if (job == 0) {
        return;                             // job 미지정은 중복 판단 안 함
    }
    s_recent.job     = job;
    s_recent.applied = applied;
    s_recent.stage   = stage;
    s_recent.pct     = pct;
    strlcpy(s_recent.ver, ver ? ver : "", sizeof(s_recent.ver));
    strlcpy(s_recent.err, err ? err : "", sizeof(s_recent.err));
    s_recent.at_us   = esp_timer_get_time();
}

// 같은 job 의 최근 결과가 RECENT_S 안에 있으면 다시 보고하고 true. applied_only = 적용 단계 결과만 본다
static bool replay_recent(uint32_t job, bool applied_only)
{
    if (job == 0 || s_recent.job != job || (applied_only && !s_recent.applied) ||
        esp_timer_get_time() - s_recent.at_us > (int64_t)RECENT_S * 1000000) {
        return false;
    }
    ESP_LOGW(TAG, "job %lu: duplicate command - repeating last result (%s)", (unsigned long)job, s_recent.stage);
    report_final(job, s_recent.stage, s_recent.pct, s_recent.ver[0] ? s_recent.ver : NULL,
                 s_recent.err[0] ? s_recent.err : NULL);
    return true;
}

// ───────── 앱 프레임 (F/R) ─────────

static char next_msg(void)
{
    s_msg = s_msg == 'F' ? '1' : (s_msg == '9' ? 'A' : s_msg + 1);
    return s_msg;
}

// F 프레임 송신 후 R 응답 대기. 0 = 성공, -1 = 응답 없음, -2 = 송신 실패(tx 큐 가득·원시 모드 등)
static int app_query(char cmd, dap_reply_t *out)
{
    uint8_t f[DAP_FRAME_MAX];
    char msg = next_msg();
    size_t n = dap_build_query(f, sizeof(f), s_pan, msg, g_cfg.agv_id, cmd);
    if (n == 0) {
        return -2;
    }
    xSemaphoreTake(s_reply_sem, 0);         // 묵은 신호 제거
    s_wait_msg = msg;
    if (!uart_bridge_send_main(f, n)) {
        s_wait_msg = 0;
        return -2;
    }
    bool got = xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(APP_REPLY_MS)) == pdTRUE;
    s_wait_msg = 0;
    if (!got || s_reply.cmd != cmd) {
        return -1;
    }
    *out = s_reply;
    return 0;
}

bool dsp_ota_observe(const uint8_t *frame, size_t len)
{
    char pan = dap_learn_pan(frame, len, g_cfg.agv_id);
    if (pan) {
        s_pan = pan;
        return false;
    }
    if (len >= 2 && frame[0] == DAP_STX && frame[1] == 'R') {
        char w = s_wait_msg;
        if (w && dap_parse_reply(frame, len, g_cfg.agv_id, w, &s_reply) == 0) {
            s_wait_msg = 0;
            xSemaphoreGive(s_reply_sem);
        }
        return true;                        // R 프레임은 서버로 올리지 않음
    }
    return false;
}

static bool query_version(void)
{
    dap_reply_t r;
    s_last_query_us = esp_timer_get_time();
    if (app_query('V', &r) != 0) {
        return false;
    }
    strlcpy(s_dsp_ver,   r.app_ver[0]  ? r.app_ver  : "-", sizeof(s_dsp_ver));
    strlcpy(s_dsp_boot,  r.boot_ver[0] ? r.boot_ver : "-", sizeof(s_dsp_boot));
    strlcpy(s_dsp_build, r.build[0]    ? r.build    : "-", sizeof(s_dsp_build));
    ESP_LOGI(TAG, "main board app %s boot %s build %s", s_dsp_ver, s_dsp_boot, s_dsp_build);
    return true;
}

// ───────── 스테이징 파티션 ─────────

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static uint32_t get32(const uint8_t *b)
{
    return b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// 결정 11: 스테이징 URL(리다이렉트 포함)의 호스트는 설정된 서버 IP 여야 함
static bool url_host_ok(const char *url)
{
    uint8_t ip[4];
    memcpy(ip, &g_cfg.server_ip, 4);        // 네트워크 바이트 순서 = a.b.c.d 순
    return g_cfg.server_ip != 0 && dap_url_host_is(url, ip);
}

static bool stage_load(void)
{
    stage_hdr_t h;
    if (esp_partition_read(s_part, 0, &h, sizeof(h)) != ESP_OK || memcmp(h.magic, STAGE_MAGIC, 8) != 0 ||
        h.file_len < PEEK_LEN || h.file_len > s_part->size - STAGE_HDR_LEN) {
        memset(&s_stage, 0, sizeof(s_stage));
        return false;
    }
    s_stage = h;
    s_stage.ver[sizeof(s_stage.ver) - 1] = '\0';
    return true;
}

static void stage_clear(void)
{
    memset(&s_stage, 0, sizeof(s_stage));
    esp_partition_erase_range(s_part, 0, 4096);
}

// .dspfw 머리(파일 헤더 32 B + 앱 헤더 128 B) 검사 → 앱 헤더 워드·구조체·버전(ver[DSPIMG_VER_WORDS]).
// 버전은 서버 DspVersion 규칙(1~8자 [0-9A-Za-z._-])이어야 하고 파일 헤더 버전(서버가 비교하는 값)과 같아야 한다
static const char *head_check(const uint8_t *b, uint32_t file_len, uint16_t *apphdr, dspimg_hdr_t *ah, char *ver)
{
    if (memcmp(b, FILE_MAGIC, 7) != 0 || get32(&b[8]) != file_len) {
        return "image_crc";
    }
    for (int i = 0; i < DSPIMG_HDR_WORDS; i++) {
        apphdr[i] = b[FILE_HDR_LEN + i * 2] | (b[FILE_HDR_LEN + i * 2 + 1] << 8);
    }
    if (dspimg_hdr_parse(apphdr, ah) != DSPIMG_OK) {
        return "image_crc";
    }
    for (int i = 0; i < DSPIMG_VER_WORDS; i++) ver[i] = (char)ah->ver[i];     // ver[31] = 0 (hdr_parse)
    char fver[16];
    memcpy(fver, &b[16], 15);
    fver[15] = '\0';
    if (!dap_ver_valid(ver) || strcmp(ver, fver) != 0) {
        return "ver_invalid";
    }
    return NULL;
}

static bool part_read(void *ctx, uint32_t off, void *buf, size_t n)
{
    (void)ctx;
    return esp_partition_read(s_part, STAGE_HDR_LEN + off, buf, n) == ESP_OK;
}

// 파티션의 .dspfw 전체 검사: 머리(형식·버전) → 파일 CRC32 → 블록(크기·주소·복원 CRC32 = app_crc).
// NULL = 정상. image_crc / ver_invalid / image_bad
static const char *stage_check(uint32_t file_len, uint32_t *file_crc, uint16_t *apphdr, dspimg_hdr_t *ah, char *ver)
{
    uint8_t hb[PEEK_LEN];
    if (file_len < PEEK_LEN || esp_partition_read(s_part, STAGE_HDR_LEN, hb, sizeof(hb)) != ESP_OK) {
        return "image_crc";
    }
    const char *err = head_check(hb, file_len, apphdr, ah, ver);
    if (err) {
        return err;
    }
    uint32_t crc = get32(&hb[12]);
    uint32_t c = dspimg_crc32_init();
    for (uint32_t off = FILE_HDR_LEN; off < file_len;) {
        size_t n = file_len - off < HTTP_BUF ? file_len - off : HTTP_BUF;
        if (esp_partition_read(s_part, STAGE_HDR_LEN + off, s_http, n) != ESP_OK) {
            return "image_crc";
        }
        static uint16_t w[HTTP_BUF];
        for (size_t i = 0; i < n; i++) w[i] = s_http[i];
        c = dspimg_crc32_bytes(c, w, n);
        off += n;
    }
    if (dspimg_crc32_final(c) != crc) {
        return "image_crc";
    }
    int rc = dap_image_check_blocks(part_read, NULL, file_len, ah->app_len, ah->app_crc);
    if (rc != DAP_IMG_OK) {
        ESP_LOGW(TAG, "image blocks bad (%d)", rc);
        return rc == DAP_IMG_E_READ ? "stage_read" : "image_bad";
    }
    *file_crc = crc;
    return NULL;
}

// ───────── 다운로드 ─────────

static const char *download(const cmd_t *c)
{
    esp_http_client_config_t hc = { .url = c->url, .timeout_ms = 10000, .buffer_size = HTTP_BUF };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h) {
        return "http_init";
    }
    const char *err = NULL;
    uint32_t pos = 0;
    int64_t clen = 0;
    int status = 0;
    // esp_http_client_open 은 3xx 를 따라가지 않으므로 Location 으로 다시 연다 (esp_https_ota 와 같은 동작)
    for (int redirects = 0; ; redirects++) {
        if (esp_http_client_open(h, 0) != ESP_OK) {
            err = "http_open";
            goto out;
        }
        clen = esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && redirects < MAX_REDIRECTS) {
            esp_http_client_set_redirection(h);
            esp_http_client_close(h);
            // 리다이렉트로 다른 호스트에 가지 않게 (결정 11)
            char cur[sizeof(c->url)];
            if (esp_http_client_get_url(h, cur, sizeof(cur)) != ESP_OK || !url_host_ok(cur)) {
                err = "url_host";
                goto out;
            }
            continue;
        }
        break;
    }
    if (status != 200) {
        err = "http_status";
        goto out;
    }
    if (clen < PEEK_LEN || clen > (int64_t)(s_part->size - STAGE_HDR_LEN) || (c->size && clen != c->size)) {
        err = "http_length";
        goto out;
    }
    // 머리(파일 헤더 + 앱 헤더)를 먼저 받아 형식·버전 검사 — 잘못된 이미지로 기존 스테이징을 지우지 않는다
    int n = 0;
    while (n < PEEK_LEN) {
        int r = esp_http_client_read(h, (char *)s_http + n, HTTP_BUF - n);
        if (r < 0) {
            err = "http_read";
            goto out;
        }
        if (r == 0) {
            break;
        }
        n += r;
    }
    if (n < PEEK_LEN) {
        err = "http_short";
        goto out;
    }
    uint16_t apphdr[DSPIMG_HDR_WORDS];
    dspimg_hdr_t ah;
    char ver[DSPIMG_VER_WORDS];
    if ((err = head_check(s_http, (uint32_t)clen, apphdr, &ah, ver)) != NULL) {
        goto out;
    }
    // 머리 섹터부터 지움 (머리가 지워지므로 다운로드 중에는 '스테이징 없음').
    // 나머지는 쓰기 직전에 4KB 씩 지운다 — 384KB 를 한 번에 지우면 그동안 캐시가 꺼져 UART 중계 태스크까지 멈춘다
    memset(&s_stage, 0, sizeof(s_stage));
    uint32_t erased = 0;
    const uint32_t need = ((uint32_t)clen + STAGE_HDR_LEN + 4095) & ~4095U;
    if (esp_partition_erase_range(s_part, 0, 4096) != ESP_OK) {
        err = "flash_erase";
        goto out;
    }
    erased = 4096;
    int last_pct = -1;
    for (;;) {
        if (n == 0) {                       // 첫 조각(머리)은 이미 s_http 에 있음
            n = esp_http_client_read(h, (char *)s_http, HTTP_BUF);
            if (n < 0) {
                err = "http_read";
                goto out;
            }
            if (n == 0) {
                break;
            }
        }
        if (pos + n > (uint32_t)clen) {
            err = "http_overrun";
            goto out;
        }
        while (erased < STAGE_HDR_LEN + pos + (uint32_t)n && erased < need) {
            if (esp_partition_erase_range(s_part, erased, 4096) != ESP_OK) {
                err = "flash_erase";
                goto out;
            }
            erased += 4096;
            vTaskDelay(1);          // 섹터 지우기 사이에 중계 태스크가 돌 틈
        }
        if (esp_partition_write(s_part, STAGE_HDR_LEN + pos, s_http, n) != ESP_OK) {
            err = "flash_write";
            goto out;
        }
        pos += n;
        n = 0;
        int pct = (int)(pos * 100 / clen);
        if (pct / 20 != last_pct / 20) {
            last_pct = pct;
            report(c->job, "download", pct, NULL, NULL);
        }
    }
    if (pos != (uint32_t)clen) {
        err = "http_short";
        goto out;
    }
    // 스테이징 머리를 쓰기 전에 전체 검사 (파일 CRC·버전·블록 구조·복원 CRC) — 적용 단계에서 지운 뒤 드러나지 않게
    uint32_t fcrc;
    if ((err = stage_check(pos, &fcrc, apphdr, &ah, ver)) != NULL) {
        goto out;
    }
    if (c->crc32 && c->crc32 != fcrc) {
        err = "crc_mismatch";
        goto out;
    }
    stage_hdr_t sh = { .job = c->job, .file_len = pos, .file_crc = fcrc, .app_crc = ah.app_crc };
    memcpy(sh.magic, STAGE_MAGIC, 8);
    strlcpy(sh.ver, ver, sizeof(sh.ver));   // 8자 이하 (head_check)
    if (esp_partition_write(s_part, 0, &sh, sizeof(sh)) != ESP_OK) {
        err = "flash_write";
        goto out;
    }
    s_stage = sh;
out:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return err;
}

// ───────── 부트로더 클라이언트 ─────────

static uint8_t s_seq;

// 요청 송신 → 응답 수신·검증 (같은 seq 로 tries 회). 반환 = 응답 payload 길이, 실패 -1 (resp 는 BP_MAX_PAYLOAD 이상)
static int bl_xfer_n(uint8_t cmd, const uint8_t *pl, size_t n, uint8_t *resp, uint32_t timeout_ms, int tries)
{
    static uint16_t tmp[BP_MAX_PACKET];
    uint16_t w[BP_MAX_PAYLOAD];
    for (size_t i = 0; i < n; i++) w[i] = pl[i];
    uint8_t seq = ++s_seq;
    uint16_t len = bp_build(tmp, cmd, seq, w, (uint16_t)n);
    uint8_t pkt[BP_MAX_PACKET];
    for (uint16_t i = 0; i < len; i++) pkt[i] = (uint8_t)tmp[i];

    for (int attempt = 0; attempt < tries; attempt++) {
        uart_bridge_raw_write(pkt, len);
        // 응답 파싱: SOF, len, body, crc
        int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        int state = 0;
        uint16_t rlen = 0, idx = 0;
        uint8_t body[BP_MAX_PACKET];
        while (esp_timer_get_time() < deadline) {
            uint8_t b;
            if (uart_bridge_raw_read(&b, 1, 50) != 1) {
                continue;
            }
            switch (state) {
            case 0: state = b == BP_SOF0 ? 1 : 0; break;
            case 1: state = b == BP_SOF1 ? 2 : (b == BP_SOF0 ? 1 : 0); break;
            case 2: rlen = b; state = 3; break;
            case 3:
                rlen |= (uint16_t)b << 8;
                if (rlen < 2 || rlen > 2 + BP_MAX_PAYLOAD) { state = 0; break; }
                idx = 0; state = 4; break;
            case 4:
                body[idx++] = b;
                if (idx == rlen + 2) {
                    uint16_t crc = body[rlen] | (body[rlen + 1] << 8);
                    for (uint16_t i = 0; i < rlen; i++) tmp[i] = body[i];
                    if (crc == bp_crc16(tmp, rlen) && body[0] == (cmd | BP_RESP) && body[1] == seq) {
                        memcpy(resp, &body[2], rlen - 2);
                        return rlen - 2;
                    }
                    state = 0;      // 다른 seq·CRC 오류: 계속 대기
                }
                break;
            }
        }
        if (tries <= RETRIES || attempt == tries - 1) {
            ESP_LOGW(TAG, "cmd 0x%02X seq %u: no response (try %d)", cmd, seq, attempt + 1);
        }
    }
    return -1;
}

static int bl_xfer(uint8_t cmd, const uint8_t *pl, size_t n, uint8_t *resp, uint32_t timeout_ms)
{
    return bl_xfer_n(cmd, pl, n, resp, timeout_ms, RETRIES);
}

// 복구 적용(결정 8B): 기동 불능 앱이라 U 도 HELLO 도 안 될 때. 부트로더는 리셋 직후 약 200ms 동안 HELLO 를 기다리므로
// 50ms 간격으로 계속 보내며 운용자의 메인보드 전원 재투입을 기다린다. 10초마다 wait_power 보고(서버 대기 연장)
static int wait_power(uint32_t job, uint8_t *resp)
{
    ESP_LOGW(TAG, "no bootloader - sending HELLO for %d s, power-cycle the main board", WAIT_POWER_S);
    for (int t = 0; t < WAIT_POWER_S; t += WAIT_POWER_REPORT_S) {
        report(job, "wait_power", -1, NULL, NULL);
        int n = bl_xfer_n(BP_CMD_HELLO, NULL, 0, resp, WAIT_POWER_HELLO_MS, WAIT_POWER_REPORT_S * 1000 / WAIT_POWER_HELLO_MS);
        if (n >= 0) {
            return n;
        }
    }
    return -1;
}

// 스테이징 이미지로 메인보드 프로그래밍 1회. NULL = 성공, 아니면 오류 문자열.
//  enter   : 앱에 U(업데이트 진입) 요청부터. false = 지운 뒤 재시도 (부트로더가 업데이트 모드에 있으므로 HELLO 부터)
//  recover : 복구 적용 — U 무응답이어도 HELLO 로 직행, HELLO 실패 시 전원 재투입 대기
//  *erased : ERASE 를 보냈으면 true (이후 실패는 메인보드에 앱이 없음 → HELLO 부터 재시도 대상)
static const char *program(uint32_t job, const uint16_t *apphdr, const dspimg_hdr_t *ah, bool enter, bool recover, bool *erased)
{
    uint8_t resp[BP_MAX_PAYLOAD];
    uint8_t pl[BP_MAX_PAYLOAD];

    // 1. 앱에 업데이트 진입 요청 (정지 상태 확인은 앱이 함).
    //    응답이 없을 때 부트로더로 직행(원시 모드 = RFID·ACS 중계 끊김)은 복구 적용이거나 앱 버전을 모를 때만
    if (enter) {
        report(job, "enter", -1, NULL, NULL);
        dap_reply_t r;
        int q = app_query('U', &r);
        if (q == -2) {
            return "uart_busy";             // 송신 실패 (tx 큐 가득 등) — 무응답과 구분, 원시 모드로 가지 않음
        }
        if (q == 0) {
            if (!r.ok) {
                static char why[24];
                snprintf(why, sizeof(why), "app_ng_%02u", r.reason);
                return why;
            }
            vTaskDelay(pdMS_TO_TICKS(BOOT_START_MS));
        } else if (recover || strcmp(s_dsp_ver, "?") == 0) {
            ESP_LOGW(TAG, "no U reply - trying bootloader directly (%s)", recover ? "recover" : "app version unknown");
        } else {
            return "app_no_reply";
        }
    }

    // 2. UART 독점 → HELLO
    if (uart_bridge_raw_begin() != ESP_OK) {
        return "uart_busy";
    }
    const char *err = NULL;
    int n = bl_xfer(BP_CMD_HELLO, NULL, 0, resp, HELLO_MS);
    if (n < 0 && recover && enter) {
        n = wait_power(job, resp);
    }
    if (n != BP_HELLO_RESP_LEN || resp[0] != BP_ST_OK) {
        err = "no_bootloader";
        goto out;
    }
    if (resp[1] != BP_PROTO_VER || (resp[2] | (resp[3] << 8)) < BP_BLOCK_WORDS) {
        // 아직 아무것도 지우지 않았다 — 부트로더에 머물지 않도록 유효한 앱으로 되돌린다
        pl[0] = 0;
        bl_xfer(BP_CMD_RESET, pl, 1, resp, RESP_MS);
        err = "boot_proto";
        goto out;
    }
    strlcpy(s_dsp_boot, (const char *)&resp[5], sizeof(s_dsp_boot));

    // 3. 지우기 — 이 뒤로는 메인보드에 앱이 없다: 실패해도 heartbeat 가 옛 버전을 보고하지 않게 '?' 로
    report(job, "erase", -1, NULL, NULL);
    pl[0] = BP_ERASE_APP;
    strlcpy(s_dsp_ver, "?", sizeof(s_dsp_ver));
    strlcpy(s_dsp_build, "?", sizeof(s_dsp_build));
    *erased = true;
    if (bl_xfer(BP_CMD_ERASE, pl, 1, resp, ERASE_MS) != 1 || resp[0] != BP_ST_OK) {
        err = "erase";
        goto out;
    }

    // 4. 블록 쓰기 (파티션에서 순서대로 읽음 — 구조는 apply 에서 지우기 전에 검사함)
    uint32_t pos = FILE_HDR_LEN + DSPIMG_HDR_WORDS * 2;
    uint32_t sent = 0, total = s_stage.file_len - pos;
    int last_pct = -1;
    while (pos < s_stage.file_len) {
        uint8_t bh[6];
        if (esp_partition_read(s_part, STAGE_HDR_LEN + pos, bh, 6) != ESP_OK) { err = "stage_read"; goto out; }
        uint16_t nw = bh[4] | (bh[5] << 8);
        if (nw == 0 || nw > BP_BLOCK_WORDS || pos + 6 + nw * 2 > s_stage.file_len) { err = "stage_block"; goto out; }
        memcpy(pl, bh, 6);
        if (esp_partition_read(s_part, STAGE_HDR_LEN + pos + 6, pl + 6, nw * 2) != ESP_OK) { err = "stage_read"; goto out; }
        int wn = bl_xfer(BP_CMD_WRITE, pl, 6 + nw * 2, resp, RESP_MS);
        if (wn != 5 || resp[0] != BP_ST_OK) {
            err = wn < 0 ? "write_timeout" : resp[0] == BP_ST_PROG_FAIL ? "program" : resp[0] == BP_ST_VERIFY_FAIL ? "verify_block" : "write";
            goto out;
        }
        pos += 6 + nw * 2;
        sent += 6 + nw * 2;
        int pct = (int)(sent * 100 / total);
        if (pct / 10 != last_pct / 10) {
            last_pct = pct;
            report(job, "progress", pct, NULL, NULL);
        }
    }

    // 5. 앱 헤더 마지막
    put32(pl, DSPIMG_APP_HDR_ADDR);
    pl[4] = DSPIMG_HDR_WORDS; pl[5] = 0;
    for (int i = 0; i < DSPIMG_HDR_WORDS; i++) { pl[6 + i * 2] = apphdr[i]; pl[7 + i * 2] = apphdr[i] >> 8; }
    if (bl_xfer(BP_CMD_WRITE, pl, 6 + DSPIMG_HDR_WORDS * 2, resp, RESP_MS) != 5 || resp[0] != BP_ST_OK) {
        err = "write_header";
        goto out;
    }

    // 6. 검증
    report(job, "verify", -1, NULL, NULL);
    if (bl_xfer(BP_CMD_VERIFY, NULL, 0, resp, 2000) != 11 || resp[0] != BP_ST_OK || resp[1] != 1 ||
        get32(&resp[2]) != ah->app_crc) {
        err = "verify";
        goto out;
    }

    // 7. 앱으로 리셋
    pl[0] = 0;
    bl_xfer(BP_CMD_RESET, pl, 1, resp, RESP_MS);
out:
    uart_bridge_raw_end();
    return err;
}

static const char *apply(uint32_t job, bool recover)
{
    uint16_t apphdr[DSPIMG_HDR_WORDS];
    dspimg_hdr_t ah;
    uint32_t fcrc;
    char newver[DSPIMG_VER_WORDS];
    if (!s_stage.magic[0]) {
        return "not_staged";
    }
    // 상태를 먼저 세운 뒤 ESP OTA 를 본다 — ota_start 도 s_busy 를 먼저 세운 뒤 이쪽 상태를 보므로 동시에 시작해도 한쪽은 물러난다
    s_state = ST_PROGRAMMING;
    // volatile 쓰기만으로는 다른 코어에서 뒤의 s_busy 읽기보다 늦게 보일 수 있다 (저장→적재 재배치).
    // 전체 펜스로 '내 상태 쓰기 → 상대 플래그 읽기' 순서를 보장해야 ota.c 의 CAS(SEQ_CST) 와 상호 배제가 두 코어에서도 성립
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ota_busy()) {
        s_state = ST_STAGED;
        return "esp_ota_busy";
    }
    // 메인보드를 지우기 전에 스테이징 이미지를 다시 검사 (파일 CRC·버전 규칙·블록 구조·복원 CRC)
    const char *err = stage_check(s_stage.file_len, &fcrc, apphdr, &ah, newver);
    if (err) {
        s_state = ST_STAGED;
        return strcmp(err, "image_crc") == 0 ? "stage_invalid" : err;
    }
    bool erased = false;
    err = program(job, apphdr, &ah, true, recover, &erased);
    // 지운 뒤 실패: 부트로더가 업데이트 모드에 머물러 있으므로 U 없이 HELLO 부터 다시 (메인보드에 앱이 없는 채로 끝내지 않게)
    for (int n = 1; err && erased && n <= PROG_RETRIES; n++) {
        ESP_LOGW(TAG, "failed after erase (%s) - retry %d from HELLO", err, n);
        report(job, "retry", n, NULL, NULL);
        err = program(job, apphdr, &ah, false, false, &erased);
    }
    if (err) {
        s_state = ST_STAGED;
        return err;
    }
    // 8. 앱 기동 확인 → 버전 비교
    strlcpy(s_dsp_ver, "?", sizeof(s_dsp_ver));
    for (int i = 0; i < APP_UP_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (query_version()) {
            break;
        }
    }
    if (strcmp(s_dsp_ver, "?") == 0) {
        s_state = ST_STAGED;
        return "app_no_reply";
    }
    // 이미지 버전은 8자 이하로 검사됨(head_check) → R 응답 버전 칸(8자)과 정확히 비교
    if (strcmp(s_dsp_ver, newver) != 0) {
        s_state = ST_STAGED;
        return "ver_mismatch";
    }
    stage_clear();
    s_state = ST_IDLE;
    return NULL;
}

// ───────── 태스크 ─────────

// 스테이징·적용 명령 1건 처리 (dsp 태스크). 같은 job 의 중복 명령(서버 재전송)은 다시 받거나 굽지 않고 결과를 다시 보고한다
static void handle_cmd(const cmd_t *c)
{
    if (c->type == CMD_QUERY_VER) {
        query_version();
        return;
    }
    if (!s_part) {
        report_final(c->job, "fail", -1, NULL, "no_partition");
        return;
    }
    if (c->type == CMD_START) {
        if (c->job != 0 && s_stage.magic[0] && s_stage.job == c->job) {
            // 이미 받아 둔 job: 다시 받지 않고 staged 를 다시 보고 (apply:1 이면 아래 적용 단계에서 다시 판단)
            ESP_LOGW(TAG, "job %lu: duplicate START - already staged", (unsigned long)c->job);
            report_final(c->job, "staged", 100, s_stage.ver, NULL);
            if (!c->apply) {
                return;
            }
        } else if (replay_recent(c->job, false)) {
            return;                             // 받기 실패·적용 완료(스테이징 지워짐) 뒤의 중복
        } else {
            if (s_state == ST_PROGRAMMING) {
                report_final(c->job, "fail", -1, NULL, "busy");
                return;
            }
            s_state = ST_DOWNLOADING;
            // 위 apply() 와 같은 이유 (ota.c 의 s_busy 와 상호 배제)
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            // 브리지 자체 OTA 가 진행 중이면 받지 않는다 (OTA 뒤 재부팅이 스테이징을 끊음). 상태를 먼저 세운 뒤 확인
            const char *err = ota_busy() ? "esp_ota_busy" : download(c);
            if (err) {
                stage_load();           // 이전 스테이징이 남아 있으면 복원
                s_state = s_stage.magic[0] ? ST_STAGED : ST_IDLE;
                report_result(c->job, false, "fail", -1, NULL, err);
                return;
            }
            s_state = ST_STAGED;
            report_result(c->job, false, "staged", 100, s_stage.ver, NULL);
            if (!c->apply) {
                return;
            }
        }
    }
    // 적용 (DSP_OTA_APPLY, 또는 START apply:1 의 스테이징 뒤)
    if (replay_recent(c->job, true)) {
        return;                                 // 같은 job 의 적용이 이미 끝남 (done 뒤 not_staged 로 오판되지 않게)
    }
    if (c->type == CMD_APPLY && s_stage.magic[0] && c->job != 0 && c->job != s_stage.job) {
        report_result(c->job, true, "fail", -1, s_stage.ver, "job_mismatch");
        return;
    }
    uint32_t job = c->type == CMD_APPLY ? s_stage.job : c->job;
    s_cur_apply = job;
    report(job, "start", 0, s_stage.ver, NULL);
    const char *err = apply(job, c->recover);
    if (err) {
        report_result(job, true, "fail", -1, s_stage.ver, err);
    } else {
        report_result(job, true, "done", 100, s_dsp_ver, NULL);
    }
    s_cur_apply = 0;
}

static void dsp_task(void *arg)
{
    cmd_t c;
    vTaskDelay(pdMS_TO_TICKS(3000));        // 메인보드 기동 대기
    query_version();
    for (;;) {
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(1000)) != pdTRUE) {
            // 버전을 모르면 주기적으로 재조회 (구 펌웨어면 계속 '?')
            if (strcmp(s_dsp_ver, "?") == 0 && s_state != ST_PROGRAMMING &&
                esp_timer_get_time() - s_last_query_us > (int64_t)VER_QUERY_RETRY_S * 1000000) {
                query_version();
            }
            continue;
        }
        if (c.type == CMD_START) {
            s_cur_start = c.job;
        }
        handle_cmd(&c);
        s_cur_start = 0;
    }
}

// ───────── 서버 명령 ─────────
// 평면 JSON (dap_json_str·dap_json_u32). 여기서의 거부 보고는 서버 수신 태스크 문맥이라 간격 없이 반복.
// 중복 명령(서버 재전송, 같은 job) 판단은 dsp 태스크(handle_cmd)가 한다 — 상태·최근 결과를 그 태스크만 쓰므로 경쟁이 없다.
// 여기서는 처리 중인 job(s_cur_start·s_cur_apply, 32비트 한 번 읽기)과 같을 때만 큐에 넣지 않고 버린다.
// 읽은 직후 처리가 끝나도 그 명령은 큐로 가서 dsp 태스크가 최근 결과를 다시 보고하므로 어느 쪽이든 결과는 같다

void dsp_ota_start(const char *json, size_t len)
{
    cmd_t c = { .type = CMD_START };
    c.job = dap_json_u32(json, len, "job");
    // 같은 job 을 받는 중(이어지는 적용 포함)이면 서버 재전송 — 무시. 그 사이 끝났으면 큐에 들어가고 dsp 태스크가 결과를 다시 보고
    if (c.job != 0 && c.job == s_cur_start) {
        ESP_LOGW(TAG, "job %lu: duplicate START ignored (in progress)", (unsigned long)c.job);
        return;
    }
    int rc = dap_json_str(json, len, "url", c.url, sizeof(c.url));
    if (rc == DAP_JSON_TOO_LONG) {
        report_final_ex(c.job, "fail", -1, NULL, "url_too_long", false);    // 잘라 쓰면 엉뚱한 파일을 받음
        return;
    }
    if (rc != DAP_JSON_OK || strncmp(c.url, "http", 4) != 0) {
        report_final_ex(c.job, "fail", -1, NULL, "bad_request", false);
        return;
    }
    if (!url_host_ok(c.url)) {
        ESP_LOGW(TAG, "staging url host is not the configured server: %s", c.url);
        report_final_ex(c.job, "fail", -1, NULL, "url_host", false);
        return;
    }
    dap_json_str(json, len, "ver", c.ver, sizeof(c.ver));
    c.crc32   = dap_json_u32(json, len, "crc32");
    c.size    = dap_json_u32(json, len, "size");
    c.apply   = dap_json_u32(json, len, "apply") != 0;
    c.recover = dap_json_u32(json, len, "recover") == 1;
    if (!s_q || xQueueSend(s_q, &c, 0) != pdTRUE) {
        report_final_ex(c.job, "fail", -1, NULL, s_q ? "busy" : "not_ready", false);
    }
}

void dsp_ota_apply(const char *json, size_t len)
{
    cmd_t c = { .type = CMD_APPLY };
    c.job     = dap_json_u32(json, len, "job");
    if (c.job != 0 && c.job == s_cur_apply) {     // 같은 job 을 굽는 중 — 서버 재전송, 무시
        ESP_LOGW(TAG, "job %lu: duplicate APPLY ignored (in progress)", (unsigned long)c.job);
        return;
    }
    c.recover = dap_json_u32(json, len, "recover") == 1;
    if (!s_q || xQueueSend(s_q, &c, 0) != pdTRUE) {
        report_final_ex(c.job, "fail", -1, NULL, s_q ? "busy" : "not_ready", false);
    }
}

void dsp_ota_query_version(void)
{
    cmd_t c = { .type = CMD_QUERY_VER };
    if (s_q) {
        xQueueSend(s_q, &c, 0);
    }
}

bool dsp_ota_busy(void)
{
    return s_state == ST_PROGRAMMING;
}

bool dsp_ota_staging(void)
{
    return s_state == ST_DOWNLOADING;
}

size_t dsp_ota_status_json(char *buf, size_t cap)
{
    // 메인보드 응답·부트로더 HELLO·스테이징에서 온 문자열 → 이스케이프
    char v[72], b[72], d[72], s[100];
    dap_json_escape(v, sizeof(v), s_dsp_ver);
    dap_json_escape(b, sizeof(b), s_dsp_boot);
    dap_json_escape(d, sizeof(d), s_dsp_build);
    int n = snprintf(buf, cap, "\"dsp\":\"%s\",\"dspb\":\"%s\",\"dsp_build\":\"%s\",\"dsp_st\":\"%s\",\"staged\":",
                     v, b, d, s_part ? ST_NAME[s_state] : "nopart");
    if (n < 0) return 0;
    if ((size_t)n < cap) {
        if (s_stage.magic[0]) {
            dap_json_escape(s, sizeof(s), s_stage.ver);
            n += snprintf(buf + n, cap - n, "{\"job\":%lu,\"ver\":\"%s\"}", (unsigned long)s_stage.job, s);
        } else {
            n += snprintf(buf + n, cap - n, "null");
        }
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t dsp_ota_format(char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "dsp : app=%s boot=%s build=%s state=%s staged=%s(job %lu) pan=%c\n",
                     s_dsp_ver, s_dsp_boot, s_dsp_build, s_part ? ST_NAME[s_state] : "nopart (USB reflash needed)",
                     s_stage.magic[0] ? s_stage.ver : "-", (unsigned long)s_stage.job, s_pan);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

esp_err_t dsp_ota_init(void)
{
    // 큐·세마포어를 먼저 만든다: 파티션이 없어도(구 파티션 테이블인 채 OTA 로 올라온 브리지) 서버 명령이 패닉 대신 거부되고 버전 조회는 된다
    s_reply_sem = xSemaphoreCreateBinary();
    QueueHandle_t q = xQueueCreate(2, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(q && s_reply_sem, ESP_ERR_NO_MEM, TAG, "queue");
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "partition '%s' not found - mainboard OTA disabled until this bridge is reflashed over USB with the new partition table", PART_LABEL);
    }
    s_q = q;
    if (s_part && stage_load()) {
        s_state = ST_STAGED;
        ESP_LOGI(TAG, "staged image: job %lu ver %s (%lu B)", (unsigned long)s_stage.job, s_stage.ver, (unsigned long)s_stage.file_len);
    }
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(dsp_task, "dsp_ota", TASK_STACK, NULL, TASK_PRIO, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
