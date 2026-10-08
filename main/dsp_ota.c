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
#define ERASE_MS            20000
#define RETRIES             3
#define APP_UP_TRIES        8           // RESET 후 앱 기동 확인 (1초 간격)
#define VER_QUERY_RETRY_S   60          // 버전 모를 때 재조회 간격

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
    char       url[200];
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

// ───────── 보고 ─────────

static void report(uint32_t job, const char *stage, int pct, const char *ver, const char *err)
{
    char js[200];
    int n = snprintf(js, sizeof(js), "{\"job\":%lu,\"stage\":\"%s\"", (unsigned long)job, stage);
    if (pct >= 0)  n += snprintf(js + n, sizeof(js) - n, ",\"pct\":%d", pct);
    if (ver)       n += snprintf(js + n, sizeof(js) - n, ",\"ver\":\"%s\"", ver);
    if (err)       n += snprintf(js + n, sizeof(js) - n, ",\"err\":\"%s\"", err);
    n += snprintf(js + n, sizeof(js) - n, "}");
    if (n > 0 && (size_t)n < sizeof(js)) {
        net_send_dsp_result((const uint8_t *)js, (size_t)n);
    }
    ESP_LOGI(TAG, "job %lu %s%s%s", (unsigned long)job, stage, err ? " err=" : "", err ? err : "");
}

// ───────── 앱 프레임 (F/R) ─────────

static char next_msg(void)
{
    s_msg = s_msg == 'F' ? '1' : (s_msg == '9' ? 'A' : s_msg + 1);
    return s_msg;
}

// F 프레임 송신 후 R 응답 대기. 0 = 성공
static int app_query(char cmd, dap_reply_t *out)
{
    uint8_t f[DAP_FRAME_MAX];
    char msg = next_msg();
    size_t n = dap_build_query(f, sizeof(f), s_pan, msg, g_cfg.agv_id, cmd);
    if (n == 0) {
        return -1;
    }
    xSemaphoreTake(s_reply_sem, 0);         // 묵은 신호 제거
    s_wait_msg = msg;
    if (!uart_bridge_send_main(f, n)) {
        s_wait_msg = 0;
        return -1;
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

static bool stage_load(void)
{
    stage_hdr_t h;
    if (esp_partition_read(s_part, 0, &h, sizeof(h)) != ESP_OK || memcmp(h.magic, STAGE_MAGIC, 8) != 0 ||
        h.file_len < FILE_HDR_LEN + DSPIMG_HDR_WORDS * 2 || h.file_len > s_part->size - STAGE_HDR_LEN) {
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

// 파티션의 .dspfw 전체 CRC 검사 + 앱 헤더 추출
static bool stage_verify(uint32_t file_len, uint32_t *file_crc, uint16_t *apphdr)
{
    uint8_t fh[FILE_HDR_LEN];
    if (esp_partition_read(s_part, STAGE_HDR_LEN, fh, sizeof(fh)) != ESP_OK || memcmp(fh, FILE_MAGIC, 7) != 0) {
        return false;
    }
    uint32_t total = fh[8] | (fh[9] << 8) | ((uint32_t)fh[10] << 16) | ((uint32_t)fh[11] << 24);
    uint32_t crc   = fh[12] | (fh[13] << 8) | ((uint32_t)fh[14] << 16) | ((uint32_t)fh[15] << 24);
    if (total != file_len) {
        return false;
    }
    uint32_t c = dspimg_crc32_init();
    for (uint32_t off = FILE_HDR_LEN; off < file_len;) {
        size_t n = file_len - off < HTTP_BUF ? file_len - off : HTTP_BUF;
        if (esp_partition_read(s_part, STAGE_HDR_LEN + off, s_http, n) != ESP_OK) {
            return false;
        }
        static uint16_t w[HTTP_BUF];
        for (size_t i = 0; i < n; i++) w[i] = s_http[i];
        c = dspimg_crc32_bytes(c, w, n);
        off += n;
    }
    if (dspimg_crc32_final(c) != crc) {
        return false;
    }
    uint8_t hb[DSPIMG_HDR_WORDS * 2];
    if (esp_partition_read(s_part, STAGE_HDR_LEN + FILE_HDR_LEN, hb, sizeof(hb)) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < DSPIMG_HDR_WORDS; i++) apphdr[i] = hb[i * 2] | (hb[i * 2 + 1] << 8);
    *file_crc = crc;
    return true;
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
    if (esp_http_client_open(h, 0) != ESP_OK) {
        err = "http_open";
        goto out;
    }
    int64_t clen = esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        err = "http_status";
        goto out;
    }
    if (clen <= 0 || clen > (int64_t)(s_part->size - STAGE_HDR_LEN) || (c->size && clen != c->size)) {
        err = "http_length";
        goto out;
    }
    // 머리 4KB 포함 전체를 지움 (머리가 지워지므로 다운로드 중에는 '스테이징 없음')
    memset(&s_stage, 0, sizeof(s_stage));
    uint32_t erase = ((uint32_t)clen + STAGE_HDR_LEN + 4095) & ~4095U;
    if (esp_partition_erase_range(s_part, 0, erase) != ESP_OK) {
        err = "flash_erase";
        goto out;
    }
    int last_pct = -1;
    for (;;) {
        int n = esp_http_client_read(h, (char *)s_http, HTTP_BUF);
        if (n < 0) {
            err = "http_read";
            goto out;
        }
        if (n == 0) {
            break;
        }
        if (pos + n > (uint32_t)clen) {
            err = "http_overrun";
            goto out;
        }
        if (esp_partition_write(s_part, STAGE_HDR_LEN + pos, s_http, n) != ESP_OK) {
            err = "flash_write";
            goto out;
        }
        pos += n;
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
    uint32_t fcrc;
    uint16_t apphdr[DSPIMG_HDR_WORDS];
    dspimg_hdr_t ah;
    if (!stage_verify(pos, &fcrc, apphdr) || dspimg_hdr_parse(apphdr, &ah) != DSPIMG_OK) {
        err = "image_crc";
        goto out;
    }
    if (c->crc32 && c->crc32 != fcrc) {
        err = "crc_mismatch";
        goto out;
    }
    stage_hdr_t sh = { .job = c->job, .file_len = pos, .file_crc = fcrc, .app_crc = ah.app_crc };
    memcpy(sh.magic, STAGE_MAGIC, 8);
    for (int i = 0; i < 15; i++) sh.ver[i] = (char)ah.ver[i];
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

// 요청 송신 → 응답 수신·검증. 반환 = 응답 payload 길이, 실패 -1 (resp 는 BP_MAX_PAYLOAD 이상)
static int bl_xfer(uint8_t cmd, const uint8_t *pl, size_t n, uint8_t *resp, uint32_t timeout_ms)
{
    static uint16_t tmp[BP_MAX_PACKET];
    uint16_t w[BP_MAX_PAYLOAD];
    for (size_t i = 0; i < n; i++) w[i] = pl[i];
    uint8_t seq = ++s_seq;
    uint16_t len = bp_build(tmp, cmd, seq, w, (uint16_t)n);
    uint8_t pkt[BP_MAX_PACKET];
    for (uint16_t i = 0; i < len; i++) pkt[i] = (uint8_t)tmp[i];

    for (int attempt = 0; attempt < RETRIES; attempt++) {
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
        ESP_LOGW(TAG, "cmd 0x%02X seq %u: no response (try %d)", cmd, seq, attempt + 1);
    }
    return -1;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static uint32_t get32(const uint8_t *b)
{
    return b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// 스테이징 이미지로 메인보드 프로그래밍. NULL = 성공, 아니면 오류 문자열
static const char *program(uint32_t job, char *newver, size_t vcap)
{
    uint8_t resp[BP_MAX_PAYLOAD];
    uint8_t pl[BP_MAX_PAYLOAD];
    uint16_t apphdr[DSPIMG_HDR_WORDS];
    uint32_t fcrc;
    dspimg_hdr_t ah;

    if (!stage_verify(s_stage.file_len, &fcrc, apphdr) || dspimg_hdr_parse(apphdr, &ah) != DSPIMG_OK) {
        return "stage_invalid";
    }
    for (int i = 0; i < DSPIMG_VER_WORDS && i < (int)vcap - 1; i++) {
        newver[i] = (char)ah.ver[i];
        newver[i + 1] = 0;
    }

    // 1. 앱에 업데이트 진입 요청 (정지 상태 확인은 앱이 함). 응답 없으면 이미 부트로더일 수 있으므로 계속
    report(job, "enter", -1, NULL, NULL);
    dap_reply_t r;
    if (app_query('U', &r) == 0) {
        if (!r.ok) {
            static char why[24];
            snprintf(why, sizeof(why), "app_ng_%02u", r.reason);
            return why;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_START_MS));
    } else {
        ESP_LOGW(TAG, "no U reply - trying bootloader directly");
    }

    // 2. UART 독점 → HELLO
    if (uart_bridge_raw_begin() != ESP_OK) {
        return "uart_busy";
    }
    const char *err = NULL;
    int n = bl_xfer(BP_CMD_HELLO, NULL, 0, resp, RESP_MS);
    if (n != BP_HELLO_RESP_LEN || resp[0] != BP_ST_OK) {
        err = "no_bootloader";
        goto out;
    }
    if (resp[1] != BP_PROTO_VER || (resp[2] | (resp[3] << 8)) < BP_BLOCK_WORDS) {
        err = "boot_proto";
        goto out;
    }
    strlcpy(s_dsp_boot, (const char *)&resp[5], sizeof(s_dsp_boot));

    // 3. 지우기
    report(job, "erase", -1, NULL, NULL);
    pl[0] = BP_ERASE_APP;
    if (bl_xfer(BP_CMD_ERASE, pl, 1, resp, ERASE_MS) != 1 || resp[0] != BP_ST_OK) {
        err = "erase";
        goto out;
    }

    // 4. 블록 쓰기 (파티션에서 순서대로 읽음)
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
        if (bl_xfer(BP_CMD_WRITE, pl, 6 + nw * 2, resp, RESP_MS) != 5 || resp[0] != BP_ST_OK) {
            err = resp[0] == BP_ST_PROG_FAIL ? "program" : resp[0] == BP_ST_VERIFY_FAIL ? "verify_block" : "write";
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
        get32(&resp[2]) != ah.app_crc) {
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

static const char *apply(uint32_t job)
{
    char newver[DSPIMG_VER_WORDS];
    if (!s_stage.magic[0]) {
        return "not_staged";
    }
    if (ota_busy()) {
        return "esp_ota_busy";
    }
    s_state = ST_PROGRAMMING;
    const char *err = program(job, newver, sizeof(newver));
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
    // R 프레임의 버전 칸은 8자 고정 → 앞 8자만 비교 (버전 문자열은 8자 이하 권장, 예: 1.2.3)
    if (strncmp(s_dsp_ver, newver, 8) != 0) {
        s_state = ST_STAGED;
        return "ver_mismatch";
    }
    stage_clear();
    s_state = ST_IDLE;
    return NULL;
}

// ───────── 태스크 ─────────

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
        switch (c.type) {
        case CMD_QUERY_VER:
            query_version();
            break;
        case CMD_START: {
            if (s_state == ST_PROGRAMMING) {
                report(c.job, "fail", -1, NULL, "busy");
                break;
            }
            s_state = ST_DOWNLOADING;
            const char *err = download(&c);
            if (err) {
                stage_load();           // 이전 스테이징이 남아 있으면 복원
                s_state = s_stage.magic[0] ? ST_STAGED : ST_IDLE;
                report(c.job, "fail", -1, NULL, err);
                break;
            }
            s_state = ST_STAGED;
            report(c.job, "staged", 100, s_stage.ver, NULL);
            if (!c.apply) {
                break;
            }
        }   /* fallthrough */
        case CMD_APPLY: {
            if (c.type == CMD_APPLY && s_stage.magic[0] && c.job != 0 && c.job != s_stage.job) {
                report(c.job, "fail", -1, s_stage.ver, "job_mismatch");
                break;
            }
            uint32_t job = c.type == CMD_APPLY ? s_stage.job : c.job;
            report(job, "start", 0, s_stage.ver, NULL);
            const char *err = apply(job);
            if (err) {
                report(job, "fail", -1, s_stage.ver, err);
            } else {
                report(job, "done", 100, s_dsp_ver, NULL);
            }
            break;
        }
        }
    }
}

// ───────── 서버 명령 ─────────
// 평면 JSON 객체 {"key":"str","key":123,...} 전용 최소 파서 (중첩·이스케이프 없음 — 서버가 그렇게 보냄)

static const char *json_find(const char *json, size_t len, const char *key)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl + 2 < len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, kl) == 0 && json[i + 1 + kl] == '"') {
            const char *p = json + i + 2 + kl;
            const char *end = json + len;
            while (p < end && (*p == ' ' || *p == ':')) {
                if (*p == ':') {
                    p++;
                    while (p < end && *p == ' ') p++;
                    return p;
                }
                p++;
            }
            return NULL;
        }
    }
    return NULL;
}

static bool json_str(const char *json, size_t len, const char *key, char *dst, size_t cap)
{
    const char *p = json_find(json, len, key);
    if (!p || *p != '"') {
        return false;
    }
    const char *end = json + len;
    size_t n = 0;
    for (p++; p < end && *p != '"' && n + 1 < cap; p++) {
        dst[n++] = *p;
    }
    dst[n] = '\0';
    return p < end && *p == '"';
}

static uint32_t json_u32(const char *json, size_t len, const char *key)
{
    const char *p = json_find(json, len, key);
    if (!p || *p < '0' || *p > '9') {
        return 0;
    }
    uint32_t v = 0;
    for (const char *end = json + len; p < end && *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (uint32_t)(*p - '0');
    }
    return v;
}

void dsp_ota_start(const char *json, size_t len)
{
    cmd_t c = { .type = CMD_START };
    if (!json_str(json, len, "url", c.url, sizeof(c.url)) || strncmp(c.url, "http", 4) != 0) {
        report(json_u32(json, len, "job"), "fail", -1, NULL, "bad_request");
        return;
    }
    json_str(json, len, "ver", c.ver, sizeof(c.ver));
    c.job   = json_u32(json, len, "job");
    c.crc32 = json_u32(json, len, "crc32");
    c.size  = json_u32(json, len, "size");
    c.apply = json_u32(json, len, "apply") != 0;
    if (xQueueSend(s_q, &c, 0) != pdTRUE) {
        report(c.job, "fail", -1, NULL, "busy");
    }
}

void dsp_ota_apply(const char *json, size_t len)
{
    cmd_t c = { .type = CMD_APPLY };
    c.job = json_u32(json, len, "job");
    if (xQueueSend(s_q, &c, 0) != pdTRUE) {
        report(c.job, "fail", -1, NULL, "busy");
    }
}

void dsp_ota_query_version(void)
{
    cmd_t c = { .type = CMD_QUERY_VER };
    xQueueSend(s_q, &c, 0);
}

bool dsp_ota_busy(void)
{
    return s_state == ST_PROGRAMMING;
}

size_t dsp_ota_status_json(char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "\"dsp\":\"%s\",\"dspb\":\"%s\",\"dsp_build\":\"%s\",\"dsp_st\":\"%s\",\"staged\":",
                     s_dsp_ver, s_dsp_boot, s_dsp_build, ST_NAME[s_state]);
    if (n < 0) return 0;
    if ((size_t)n < cap) {
        n += s_stage.magic[0]
             ? snprintf(buf + n, cap - n, "{\"job\":%lu,\"ver\":\"%s\"}", (unsigned long)s_stage.job, s_stage.ver)
             : snprintf(buf + n, cap - n, "null");
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t dsp_ota_format(char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "dsp : app=%s boot=%s build=%s state=%s staged=%s(job %lu) pan=%c\n",
                     s_dsp_ver, s_dsp_boot, s_dsp_build, ST_NAME[s_state],
                     s_stage.magic[0] ? s_stage.ver : "-", (unsigned long)s_stage.job, s_pan);
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

esp_err_t dsp_ota_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "partition '%s' not found (partitions.csv)", PART_LABEL);
    s_q = xQueueCreate(2, sizeof(cmd_t));
    s_reply_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_q && s_reply_sem, ESP_ERR_NO_MEM, TAG, "queue");
    if (stage_load()) {
        s_state = ST_STAGED;
        ESP_LOGI(TAG, "staged image: job %lu ver %s (%lu B)", (unsigned long)s_stage.job, s_stage.ver, (unsigned long)s_stage.file_len);
    }
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(dsp_task, "dsp_ota", TASK_STACK, NULL, TASK_PRIO, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
