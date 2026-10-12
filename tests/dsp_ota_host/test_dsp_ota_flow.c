// main/dsp_ota.c 흐름 시험 (호스트): dsp_ota.c 를 그대로 포함하고 ESP-IDF·UART·HTTP·파티션을 가짜로 바꾼다.
// 메인보드 쪽은 실제 부트로더 프로토콜 처리기(dsp/common/bootproto.c)와 RAM 플래시, 간단한 앱 응답기로 흉내 낸다.
//   ./run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/dsp_ota.c"

int g_verbose;
app_cfg_t g_cfg;
static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

// ───────── 가짜 시계·RTOS ─────────

static int64_t m_now;                               // us
int64_t esp_timer_get_time(void) { return m_now; }
void vTaskDelay(TickType_t t) { m_now += (int64_t)(t ? t : 1) * 1000; }
static int m_sem;
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &m_sem; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    if (m_sem) { m_sem = 0; return pdTRUE; }
    m_now += (int64_t)t * 1000;
    return 0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { m_sem = 1; return pdTRUE; }
static int m_queued;
static cmd_t m_last_cmd;
QueueHandle_t xQueueCreate(int n, int sz) { return &m_queued; }
BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t t) { memcpy(&m_last_cmd, p, sizeof(cmd_t)); m_queued++; return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q, void *p, TickType_t t) { return 0; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t st, void *a, int prio, void *h, int core) { return pdPASS; }
bool ota_busy(void) { return false; }

// ───────── 보고 수집 ─────────

static char m_rep[2000][160];
static int m_nrep;
bool net_send_dsp_result(const uint8_t *d, size_t n)
{
    if (m_nrep < 2000) { memcpy(m_rep[m_nrep], d, n < 159 ? n : 159); m_rep[m_nrep][n < 159 ? n : 159] = 0; m_nrep++; }
    if (g_verbose) printf("  [rep] %.*s\n", (int)n, d);
    return true;
}
static int count_rep(const char *sub)
{
    int c = 0;
    for (int i = 0; i < m_nrep; i++) if (strstr(m_rep[i], sub)) c++;
    return c;
}

// ───────── 스테이징 파티션 (RAM) ─────────

#define PART_SIZE 0x60000
static uint8_t m_part[PART_SIZE];
static esp_partition_t m_p = { PART_SIZE };
const esp_partition_t *esp_partition_find_first(int t, int st, const char *l) { return &m_p; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *d, size_t n)
{ if (off + n > PART_SIZE) return ESP_FAIL; memcpy(d, m_part + off, n); return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *s, size_t n)
{ if (off + n > PART_SIZE) return ESP_FAIL; memcpy(m_part + off, s, n); return ESP_OK; }
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{ if (off + n > PART_SIZE) return ESP_FAIL; memset(m_part + off, 0xFF, n); return ESP_OK; }

// ───────── 메인보드 흉내: 앱 + 부트로더(bootproto.c) ─────────

// APP_SILENT: 앱 멈춤 (F 무응답, 앱 프레임 없음) / APP_OLD: 살아 있는 구 앱 (F 를 모름, E/A 프레임은 계속 보냄)
typedef enum { APP_OK, APP_SILENT, APP_NG, APP_DEAD, APP_OLD } app_mode_t;
static app_mode_t m_app;        // 앱 동작 (BOOT 가 아닐 때)
static bool m_boot;             // 부트로더 업데이트 모드
static bool m_tx_ok = true;     // uart_bridge_send_main 성공 여부 (false = tx 큐 가득)
static int64_t m_power_on_at = -1;   // APP_DEAD: 이 시각에 전원 재투입 → 200ms 동안 HELLO 를 받으면 부트로더
static int m_raw_begins;
static int m_hellos;            // 원시 모드로 보낸 HELLO 패킷 수
static int m_drop_write_resp;   // WRITE 응답을 이 횟수만큼 버림 (노이즈)
static int m_drop_after_writes; // WRITE 이 횟수 이후부터 버리기 시작
static int m_writes;
static char m_app_ver[9] = "1.0";

static uint16_t m_flash[0xC000];    // [0x3E8000, 0x3F4000)
static uint16_t f_erase(uint16_t mask) { for (int i = 0; i < 0xC000; i++) m_flash[i] = 0xFFFF; return BP_ST_OK; }
static uint16_t f_prog(uint32_t a, const uint16_t *w, uint16_t n) { memcpy(&m_flash[a - DSPIMG_APP_HDR_ADDR], w, n * 2); return BP_ST_OK; }
static void f_read(uint32_t a, uint16_t *w, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint32_t x = a + i;
        w[i] = x >= DSPIMG_APP_HDR_ADDR && x < DSPIMG_APP_END ? m_flash[x - DSPIMG_APP_HDR_ADDR] : 0xFFFF;
    }
}
static const uint16_t BOOTVER[16] = { 'B', '1', 0 };
static const bp_ops_t m_ops = { f_erase, f_prog, f_read, BOOTVER };
static bp_ctx_t m_bp;
static uint8_t m_rxq[4096];
static int m_rxh, m_rxt;

static void app_reply(char msg, const char *body)
{
    uint8_t f[64];
    size_t n = 0;
    f[n++] = DAP_STX; f[n++] = 'R'; f[n++] = '1'; f[n++] = (uint8_t)msg;
    f[n++] = (uint8_t)('0' + g_cfg.agv_id / 10); f[n++] = (uint8_t)('0' + g_cfg.agv_id % 10);
    memcpy(f + n, body, strlen(body)); n += strlen(body);
    f[n++] = ' ';
    f[n] = (uint8_t)dap_bcc(f + 1, n - 1); n++;
    f[n++] = DAP_ETX;
    dsp_ota_observe(f, n);
}

bool uart_bridge_send_main(const uint8_t *d, size_t n)
{
    if (!m_tx_ok) return false;
    if (m_boot || n < 8 || d[1] != 'F') return true;
    char msg = (char)d[3], cmd = (char)d[6];
    if (m_app == APP_SILENT || m_app == APP_DEAD || m_app == APP_OLD) return true;
    if (cmd == 'V') {
        char b[32];
        snprintf(b, sizeof(b), "V%-8s%-8s%-8s", m_app_ver, "B1", "20261011");
        app_reply(msg, b);
    } else if (cmd == 'U') {
        if (m_app == APP_NG) { app_reply(msg, "UNG01"); return true; }
        app_reply(msg, "UOK00");
        m_boot = true;                  // 정지 확인 OK → 부트플래그 → 부트로더
        bp_init(&m_bp, &m_ops);
    }
    return true;
}

// 앱 프레임 수신 경과: 살아 있는 앱(부트로더 아님)은 E/A 프레임을 계속 보낸다
uint32_t uart_bridge_main_rx_age_ms(void)
{
    bool alive = !m_boot && (m_app == APP_OK || m_app == APP_NG || m_app == APP_OLD);
    return alive ? 300 : UINT32_MAX;
}

esp_err_t uart_bridge_raw_begin(void) { m_raw_begins++; return ESP_OK; }
void uart_bridge_raw_end(void) {}

static void (*m_on_raw)(void);      // 원시 송신 때 1회 불림 (굽는 중 서버 재전송 흉내)
void uart_bridge_raw_write(const uint8_t *b, size_t n)
{
    if (m_on_raw) { void (*f)(void) = m_on_raw; m_on_raw = NULL; f(); }
    if (n >= 5 && b[0] == BP_SOF0 && b[1] == BP_SOF1 && b[4] == BP_CMD_HELLO) m_hellos++;
    if (!m_boot && m_app == APP_DEAD && m_power_on_at >= 0 && m_now >= m_power_on_at && m_now < m_power_on_at + 200000) {
        m_boot = true;                  // 리셋 직후 200ms 안에 HELLO → 업데이트 모드 (결정 8B)
        bp_init(&m_bp, &m_ops);
    }
    if (!m_boot) return;
    for (size_t i = 0; i < n; i++) {
        if (bp_rx_byte(&m_bp, b[i])) {
            bool drop = false;
            if (b[4] == BP_CMD_WRITE) {
                m_writes++;
                if (m_writes > m_drop_after_writes && m_drop_write_resp > 0) { m_drop_write_resp--; drop = true; }
            }
            if (!drop) for (uint16_t k = 0; k < m_bp.tx_len; k++) m_rxq[(m_rxt++) & 4095] = (uint8_t)m_bp.tx[k];
            if (m_bp.reset_pending) {
                m_bp.reset_pending = 0;
                dspimg_hdr_t h;
                if (bp_app_check(&m_ops, &h, NULL) == DSPIMG_OK) {
                    m_boot = false;
                    m_app = APP_OK;
                    for (int k = 0; k < 8; k++) m_app_ver[k] = (char)h.ver[k];
                    m_app_ver[8] = 0;
                }
            }
        }
    }
}

int uart_bridge_raw_read(uint8_t *b, size_t n, uint32_t to)
{
    if (m_rxh == m_rxt) { m_now += (int64_t)to * 1000; return 0; }
    b[0] = m_rxq[(m_rxh++) & 4095];
    return 1;
}

// ───────── 가짜 HTTP ─────────

static const uint8_t *m_http_body;
static uint32_t m_http_len, m_http_pos;
static char m_http_url[256];
static const char *m_redirect_to;   // 첫 요청을 302 로 이 URL 에 보냄
static bool m_redirected;
static int m_http_inits;
static void (*m_on_http_read)(void);    // HTTP 읽기 때 1회 불림 (받는 중 서버 재전송 흉내)
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{ m_http_inits++; strncpy(m_http_url, c->url, sizeof(m_http_url) - 1); m_http_pos = 0; m_redirected = false; return (esp_http_client_handle_t)1; }
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int w) { m_http_pos = 0; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) { return m_redirect_to && !m_redirected ? 0 : m_http_len; }
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return m_redirect_to && !m_redirected ? 302 : 200; }
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t h)
{ strncpy(m_http_url, m_redirect_to, sizeof(m_http_url) - 1); m_redirected = true; return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t h) { return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { return ESP_OK; }
esp_err_t esp_http_client_get_url(esp_http_client_handle_t h, char *u, const int len)
{ if ((int)strlen(m_http_url) >= len) return ESP_FAIL; strcpy(u, m_http_url); return ESP_OK; }
int esp_http_client_read(esp_http_client_handle_t h, char *b, int len)
{
    if (m_on_http_read) { void (*f)(void) = m_on_http_read; m_on_http_read = NULL; f(); }
    uint32_t n = m_http_len - m_http_pos;
    if (n > 1000) n = 1000;             // 작은 조각으로 (머리 검사가 여러 조각을 모으는지)
    if ((uint32_t)len < n) n = (uint32_t)len;
    memcpy(b, m_http_body + m_http_pos, n);
    m_http_pos += n;
    return (int)n;
}

// ───────── .dspfw 생성 (dsp_pack.py pack 과 같은 배치) ─────────

static uint8_t m_img[200000];
static void p16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// block_words: 블록 크기 (정상 128), fver: 파일 헤더 버전 (NULL = ver)
static uint32_t mkimg(const char *ver, const char *fver, uint32_t app_len, int block_words)
{
    static uint16_t flat[0xC000];
    for (uint32_t i = 0; i < app_len; i++) flat[i] = (uint16_t)(i * 13 + 7);
    dspimg_hdr_t h = { .app_len = app_len, .build_time = 1700000000 };
    h.app_crc = dspimg_crc32_final(dspimg_crc32_words(dspimg_crc32_init(), flat, app_len));
    for (int i = 0; ver[i] && i < 31; i++) h.ver[i] = (uint8_t)ver[i];
    uint16_t hdr[64];
    dspimg_hdr_build(&h, hdr);
    uint32_t pos = 32;
    for (int i = 0; i < 64; i++, pos += 2) p16(m_img + pos, hdr[i]);
    for (uint32_t off = 0; off < app_len; off += block_words) {
        uint32_t nw = app_len - off < (uint32_t)block_words ? app_len - off : (uint32_t)block_words;
        p32(m_img + pos, DSPIMG_APP_CODE_ADDR + off); p16(m_img + pos + 4, (uint16_t)nw); pos += 6;
        for (uint32_t i = 0; i < nw; i++, pos += 2) p16(m_img + pos, flat[off + i]);
    }
    memcpy(m_img, "AGVDSP1", 8);
    p32(m_img + 8, pos);
    memset(m_img + 16, 0, 16);
    strncpy((char *)m_img + 16, fver ? fver : ver, 15);
    uint16_t w[4096];
    uint32_t c = dspimg_crc32_init();
    for (uint32_t o = 32; o < pos; o += 4096) {
        uint32_t n = pos - o < 4096 ? pos - o : 4096;
        for (uint32_t i = 0; i < n; i++) w[i] = m_img[o + i];
        c = dspimg_crc32_bytes(c, w, n);
    }
    p32(m_img + 12, dspimg_crc32_final(c));
    return pos;
}

// 다운로드 없이 파티션에 직접 스테이징 (구 펌웨어가 받아 둔 이미지 흉내)
static void stage_raw(uint32_t len, uint32_t job)
{
    memset(m_part, 0xFF, sizeof(m_part));
    memcpy(m_part + STAGE_HDR_LEN, m_img, len);
    stage_hdr_t sh = { .job = job, .file_len = len };
    memcpy(sh.magic, STAGE_MAGIC, 8);
    memcpy(sh.ver, m_img + 16, 15);
    memcpy(m_part, &sh, sizeof(sh));
    stage_load();
}

static void reset_board(app_mode_t app, const char *ver)
{
    m_app = app; m_boot = false; m_tx_ok = true; m_power_on_at = -1; m_raw_begins = 0; m_hellos = 0;
    m_drop_write_resp = 0; m_drop_after_writes = 0; m_writes = 0; m_rxh = m_rxt = 0;
    strcpy(m_app_ver, ver);
    for (int i = 0; i < 0xC000; i++) m_flash[i] = 0x1111;     // 옛 앱 (지워지면 0xFFFF)
    m_nrep = 0;
    strlcpy(s_dsp_ver, ver, sizeof(s_dsp_ver));
    strlcpy(s_dsp_build, strcmp(ver, "?") == 0 ? "?" : "20261011", sizeof(s_dsp_build));
}

static bool s_flash_untouched(void);

static const char *do_download(const char *url, uint32_t len, uint32_t job)
{
    cmd_t c = { .type = CMD_START, .job = job };
    strlcpy(c.url, url, sizeof(c.url));
    m_http_body = m_img; m_http_len = len;
    const char *err = download(&c);
    if (err) stage_load();
    return err;
}

#define URL "http://192.168.0.10:8080/fw/20261011-000000-abcdef/app.dspfw"

static void t_download(void)
{
    printf("-- download / B-03 / D-08 / X-09 / redirect\n");
    uint32_t n = mkimg("1.2.3", NULL, 3000, 128);
    reset_board(APP_OK, "1.0");
    CHECK(do_download(URL, n, 5) == NULL && s_stage.magic[0] && strcmp(s_stage.ver, "1.2.3") == 0);
    // 10자 버전 → ver_invalid, 기존 스테이징(job 5)은 그대로
    n = mkimg("2026.10.11", NULL, 3000, 128);
    const char *e = do_download(URL, n, 6);
    CHECK(e && strcmp(e, "ver_invalid") == 0);
    CHECK(s_stage.magic[0] && s_stage.job == 5);
    // 앱 헤더와 파일 헤더 버전이 다름 → ver_invalid
    n = mkimg("1.2.3", "1.2.4", 3000, 128);
    e = do_download(URL, n, 7);
    CHECK(e && strcmp(e, "ver_invalid") == 0);
    // 256 워드 블록 (파일 CRC 정상) → image_bad
    n = mkimg("1.2.4", NULL, 3000, 256);
    e = do_download(URL, n, 8);
    CHECK(e && strcmp(e, "image_bad") == 0);
    // 다른 호스트로 리다이렉트 → url_host
    n = mkimg("1.2.4", NULL, 3000, 128);
    m_redirect_to = "http://10.0.0.1/evil.dspfw";
    e = do_download(URL, n, 9);
    CHECK(e && strcmp(e, "url_host") == 0);
    m_redirect_to = "http://192.168.0.10:8080/fw/x/y.dspfw";     // 같은 서버 안 리다이렉트는 허용
    CHECK(do_download(URL, n, 10) == NULL && s_stage.job == 10);
    m_redirect_to = NULL;
}

static void t_apply_basic(void)
{
    printf("-- apply: 정상 / B-01 / app_ng\n");
    uint32_t n = mkimg("1.2.3", NULL, 3000, 128);
    reset_board(APP_OK, "1.0");
    stage_raw(n, 11);
    CHECK(apply(11, false) == NULL);
    CHECK(strcmp(s_dsp_ver, "1.2.3") == 0 && !s_stage.magic[0]);
    CHECK(count_rep("\"retry\"") == 0);

    // B-01: 버전을 아는 앱이 U 무응답 → app_no_reply, 원시 모드 안 들어감, 스테이징 유지
    reset_board(APP_SILENT, "1.0");
    stage_raw(n, 12);
    const char *e = apply(12, false);
    CHECK(e && strcmp(e, "app_no_reply") == 0);
    CHECK(m_raw_begins == 0 && s_stage.magic[0] && s_flash_untouched());
    CHECK(strcmp(s_dsp_ver, "?") == 0 && strcmp(s_dsp_build, "?") == 0);     // R1: 옛 버전을 계속 보고하지 않음
    // B-01: 송신 실패 (tx 큐 가득) → uart_busy 즉시, 원시 모드 안 들어감
    reset_board(APP_OK, "1.0");
    m_tx_ok = false;
    stage_raw(n, 13);
    e = apply(13, false);
    CHECK(e && strcmp(e, "uart_busy") == 0 && m_raw_begins == 0);
    // 버전 모름('?') + 이미 부트로더 → HELLO 직행으로 이어서 완료
    reset_board(APP_SILENT, "?");
    m_boot = true; bp_init(&m_bp, &m_ops);
    stage_raw(n, 14);
    CHECK(apply(14, false) == NULL && strcmp(s_dsp_ver, "1.2.3") == 0);
    // recover=1 + 이미 부트로더 (버전을 알아도 직행)
    reset_board(APP_SILENT, "1.0");
    m_boot = true; bp_init(&m_bp, &m_ops);
    stage_raw(n, 15);
    CHECK(apply(15, true) == NULL);
    // 앱 NG (주행 중) → app_ng_01, 재시도 없음
    reset_board(APP_NG, "1.0");
    stage_raw(n, 16);
    e = apply(16, false);
    CHECK(e && strcmp(e, "app_ng_01") == 0 && count_rep("\"retry\"") == 0 && m_raw_begins == 0);
}

static bool s_flash_untouched(void)
{
    for (int i = 0; i < 0xC000; i++) if (m_flash[i] != 0x1111) return false;
    return true;
}

static void t_apply_validate(void)
{
    printf("-- apply: 지우기 전 재검사 (D-08 / X-09 / B-03)\n");
    // 구 펌웨어가 받아 둔 결함 이미지: 256 워드 블록
    uint32_t n = mkimg("1.2.3", NULL, 3000, 256);
    reset_board(APP_OK, "1.0");
    stage_raw(n, 20);
    const char *e = apply(20, false);
    CHECK(e && strcmp(e, "image_bad") == 0 && m_raw_begins == 0 && s_flash_untouched());
    // 8자 넘는 버전으로 받아 둔 이미지
    n = mkimg("2026.10.11", NULL, 3000, 128);
    reset_board(APP_OK, "1.0");
    stage_raw(n, 21);
    e = apply(21, false);
    CHECK(e && strcmp(e, "ver_invalid") == 0 && m_raw_begins == 0 && s_flash_untouched());
    // 파티션 내용 손상 → stage_invalid
    n = mkimg("1.2.3", NULL, 3000, 128);
    reset_board(APP_OK, "1.0");
    stage_raw(n, 22);
    m_part[STAGE_HDR_LEN + 500] ^= 1;
    e = apply(22, false);
    CHECK(e && strcmp(e, "stage_invalid") == 0 && m_raw_begins == 0);
}

static void t_retry(void)
{
    printf("-- apply: 지운 뒤 재시도 (D-02)\n");
    uint32_t n = mkimg("1.2.3", NULL, 3000, 128);
    // 블록 5 이후 WRITE 응답 3회 연속 유실 → write_timeout → HELLO 부터 재시도 1회로 완료
    reset_board(APP_OK, "1.0");
    m_drop_after_writes = 5; m_drop_write_resp = 3;
    stage_raw(n, 30);
    CHECK(apply(30, false) == NULL && strcmp(s_dsp_ver, "1.2.3") == 0);
    CHECK(count_rep("\"stage\":\"retry\",\"pct\":1") == 1 && count_rep("\"retry\"") == 1);
    CHECK(count_rep("\"enter\"") == 1);             // 재시도는 U 없이
    // 계속 실패 → 재시도 2회 후 fail, 스테이징 유지
    reset_board(APP_OK, "1.0");
    m_drop_after_writes = 5; m_drop_write_resp = 1000000;
    stage_raw(n, 31);
    const char *e = apply(31, false);
    CHECK(e && strcmp(e, "write_timeout") == 0);
    CHECK(count_rep("\"retry\"") == 2 && count_rep("\"pct\":2}") >= 1 && s_stage.magic[0]);
    CHECK(strcmp(s_dsp_ver, "?") == 0);
    // 그 상태(앱 지워짐, 버전 '?')에서 다시 APPLY → U 무응답이어도 HELLO 로 이어서 완료
    m_drop_write_resp = 0; m_nrep = 0; m_app = APP_SILENT;
    CHECK(apply(31, false) == NULL && strcmp(s_dsp_ver, "1.2.3") == 0);
}

static void t_wait_power(void)
{
    printf("-- apply: 복구 적용 전원 재투입 대기 (결정 8B)\n");
    uint32_t n = mkimg("1.2.3", NULL, 3000, 128);
    // 기동 불능 앱: U·HELLO 무응답. 25초 뒤 전원 재투입 → 200ms 창 안의 HELLO 로 잡힘
    reset_board(APP_DEAD, "1.0");
    stage_raw(n, 40);
    int64_t t0 = m_now;
    m_power_on_at = m_now + 25 * 1000000LL;
    CHECK(apply(40, true) == NULL && strcmp(s_dsp_ver, "1.2.3") == 0);
    CHECK(count_rep("\"wait_power\"") >= 2 && count_rep("\"wait_power\"") <= 4);
    CHECK(m_now - t0 < 60 * 1000000LL);
    // 끝내 전원 재투입 없음 → no_bootloader, 약 60초 동안 wait_power 6회
    reset_board(APP_DEAD, "1.0");
    stage_raw(n, 41);
    t0 = m_now;
    const char *e = apply(41, true);
    CHECK(e && strcmp(e, "no_bootloader") == 0 && count_rep("\"wait_power\"") == 6);
    CHECK(m_now - t0 >= 60 * 1000000LL && m_now - t0 < 75 * 1000000LL && s_flash_untouched());
    // recover 없이 기동 불능 앱 (버전 '?'): HELLO 3회 후 no_bootloader, 대기 없음
    reset_board(APP_DEAD, "?");
    stage_raw(n, 42);
    e = apply(42, false);
    CHECK(e && strcmp(e, "no_bootloader") == 0 && count_rep("\"wait_power\"") == 0);
}

static void t_alive_check(void)
{
    printf("-- U 무응답 때 앱 생존 판단 (R1 / R15)\n");
    uint32_t n = mkimg("1.2.3", NULL, 3000, 128);
    // (a) 살아 있는 구 앱 (프레임 수신 중) + U 무응답 → app_no_fwup, 원시 모드 0회 — recover 여도
    reset_board(APP_OLD, "?");
    stage_raw(n, 80);
    int64_t t0 = m_now;
    const char *e = apply(80, true);
    CHECK(e && strcmp(e, "app_no_fwup") == 0);
    CHECK(m_raw_begins == 0 && m_hellos == 0 && count_rep("\"wait_power\"") == 0 && s_flash_untouched());
    CHECK(s_stage.magic[0] && m_now - t0 < 3 * 1000000LL);
    reset_board(APP_OLD, "?");
    stage_raw(n, 81);
    e = apply(81, false);
    CHECK(e && strcmp(e, "app_no_fwup") == 0 && m_raw_begins == 0);
    reset_board(APP_OLD, "1.2.0");                  // 버전을 알아도 같음, 앱이 살아 있으므로 버전 유지
    stage_raw(n, 82);
    e = apply(82, false);
    CHECK(e && strcmp(e, "app_no_fwup") == 0 && m_raw_begins == 0 && strcmp(s_dsp_ver, "1.2.0") == 0);

    // (b) 앱 침묵 + 버전 앎 + recover 없음 → app_no_reply, 버전 '?' (heartbeat 가 '?' 보고)
    reset_board(APP_SILENT, "1.2.0");
    stage_raw(n, 83);
    e = apply(83, false);
    CHECK(e && strcmp(e, "app_no_reply") == 0 && m_raw_begins == 0);
    CHECK(strcmp(s_dsp_ver, "?") == 0 && strcmp(s_dsp_build, "?") == 0);
    char b[300];
    dsp_ota_status_json(b, sizeof(b));
    CHECK(strstr(b, "\"dsp\":\"?\"") != NULL);
    //     R1 시나리오: 멈춘 앱이 워치독 리셋을 반복해 부트로더가 업데이트 모드에 있음 → 다음 배포('?')는 HELLO 로 바로 구움
    m_boot = true; bp_init(&m_bp, &m_ops); m_nrep = 0;
    CHECK(apply(83, false) == NULL && strcmp(s_dsp_ver, "1.2.3") == 0);
    //     앱이 실제로는 살아 있었다면 재조회로 버전이 돌아옴
    reset_board(APP_SILENT, "1.2.0");
    stage_raw(n, 84);
    e = apply(84, false);
    CHECK(e && strcmp(e, "app_no_reply") == 0 && strcmp(s_dsp_ver, "?") == 0);
    m_app = APP_OK;
    CHECK(query_version() && strcmp(s_dsp_ver, "1.2.0") == 0 && strcmp(s_dsp_build, "20261011") == 0);
    //     버전 조회 무응답 → '?' (수동 dsp ver 도 옛 버전을 남기지 않음)
    m_app = APP_SILENT;
    CHECK(!query_version() && strcmp(s_dsp_ver, "?") == 0 && strcmp(s_dsp_build, "?") == 0);

    // (c) 앱 침묵 + '?' + recover 없음 → 원시 모드 HELLO 3회만 (약 4.5초), wait_power 없음
    reset_board(APP_DEAD, "?");
    stage_raw(n, 85);
    t0 = m_now;
    e = apply(85, false);
    CHECK(e && strcmp(e, "no_bootloader") == 0);
    CHECK(m_raw_begins == 1 && m_hellos == RETRIES && count_rep("\"wait_power\"") == 0);
    CHECK(m_now - t0 < 8 * 1000000LL && s_flash_untouched());

    // (d) 앱 침묵 + recover → HELLO 3회 뒤 wait_power (버전을 알아도)
    reset_board(APP_DEAD, "1.2.0");
    stage_raw(n, 86);
    m_power_on_at = m_now + 15 * 1000000LL;
    CHECK(apply(86, true) == NULL && strcmp(s_dsp_ver, "1.2.3") == 0);
    CHECK(m_raw_begins == 1 && count_rep("\"wait_power\"") >= 1 && m_hellos > RETRIES);
}

static void t_start_cmd(void)
{
    printf("-- DSP_OTA_START 검사 (C-09 / 결정 11)\n");
    char js[400], url[300];
    memset(url, 'a', sizeof(url));
    memcpy(url, "http://192.168.0.10:8080/fw/", 28);
    url[250] = 0;
    snprintf(js, sizeof(js), "{\"job\":50,\"url\":\"%s\",\"crc32\":1}", url);
    m_nrep = 0; m_queued = 0;
    dsp_ota_start(js, strlen(js));
    CHECK(m_queued == 0 && count_rep("\"err\":\"url_too_long\"") == FINAL_REPORTS && count_rep("\"job\":50") == FINAL_REPORTS);
    url[199] = 0;                                   // 정확히 199자 → 통과
    snprintf(js, sizeof(js), "{\"job\":51,\"url\":\"%s\",\"recover\":1}", url);
    dsp_ota_start(js, strlen(js));
    CHECK(m_queued == 1 && strcmp(m_last_cmd.url, url) == 0 && m_last_cmd.recover);
    snprintf(js, sizeof(js), "{\"job\":52,\"url\":\"http://192.168.0.11:8080/fw/a/b.dspfw\"}");
    m_nrep = 0;
    dsp_ota_start(js, strlen(js));
    CHECK(m_queued == 1 && count_rep("\"err\":\"url_host\"") == FINAL_REPORTS);
    snprintf(js, sizeof(js), "{\"job\":53,\"url\":\"ftp://x\"}");
    m_nrep = 0;
    dsp_ota_start(js, strlen(js));
    CHECK(count_rep("\"err\":\"bad_request\"") == FINAL_REPORTS);
    snprintf(js, sizeof(js), "{\"job\":54,\"recover\":1}");
    dsp_ota_apply(js, strlen(js));
    CHECK(m_queued == 2 && m_last_cmd.type == CMD_APPLY && m_last_cmd.recover && m_last_cmd.job == 54);
    snprintf(js, sizeof(js), "{\"job\":55}");
    dsp_ota_apply(js, strlen(js));
    CHECK(!m_last_cmd.recover);
}

// ───────── 같은 job 의 중복 명령 (INT-C1) ─────────

// dsp_task 의 한 바퀴와 같게: 큐에서 꺼낸 명령 처리
static void run_cmd(const cmd_t *in)
{
    cmd_t c = *in;
    if (c.type == CMD_START) s_cur_start = c.job;
    handle_cmd(&c);
    s_cur_start = 0;
}

static char m_js_start[300], m_js_apply[100];
static int m_q_in_hook;
static void dup_start_hook(void) { int q = m_queued; dsp_ota_start(m_js_start, strlen(m_js_start)); m_q_in_hook = m_queued - q; }
static void dup_apply_hook(void) { int q = m_queued; dsp_ota_apply(m_js_apply, strlen(m_js_apply)); m_q_in_hook = m_queued - q; }

// 서버 명령 → 큐에 들어가면 처리. 큐에 들어갔으면 true
static bool server_start(uint32_t job, int apply)
{
    snprintf(m_js_start, sizeof(m_js_start), "{\"job\":%u,\"url\":\"%s\",\"apply\":%d}", job, URL, apply);
    int q = m_queued;
    dsp_ota_start(m_js_start, strlen(m_js_start));
    if (m_queued == q) return false;
    run_cmd(&m_last_cmd);
    return true;
}
static bool server_apply(uint32_t job)
{
    snprintf(m_js_apply, sizeof(m_js_apply), "{\"job\":%u}", job);
    int q = m_queued;
    dsp_ota_apply(m_js_apply, strlen(m_js_apply));
    if (m_queued == q) return false;
    run_cmd(&m_last_cmd);
    return true;
}

static void t_duplicate(void)
{
    printf("-- 같은 job 의 중복 START·APPLY (INT-C1)\n");
    uint32_t n = mkimg("1.2.3", NULL, 3000, 128);
    reset_board(APP_OK, "1.0");
    stage_clear();
    s_state = ST_IDLE;
    memset(&s_recent, 0, sizeof(s_recent));
    m_http_body = m_img; m_http_len = n;

    // 받는 중 같은 job 의 START 재전송 → 큐에 넣지 않고 무시
    m_on_http_read = dup_start_hook; m_q_in_hook = -1;
    int inits = m_http_inits;
    CHECK(server_start(70, 0));
    CHECK(m_q_in_hook == 0 && m_http_inits == inits + 1);
    CHECK(s_stage.magic[0] && s_stage.job == 70 && s_state == ST_STAGED);
    CHECK(count_rep("\"job\":70,\"stage\":\"staged\"") == FINAL_REPORTS);
    // 스테이징 완료 뒤 중복 START → 다시 받지 않고 staged 다시 보고
    m_nrep = 0;
    CHECK(server_start(70, 0));
    CHECK(m_http_inits == inits + 1 && count_rep("\"download\"") == 0);
    CHECK(count_rep("\"job\":70,\"stage\":\"staged\",\"pct\":100,\"ver\":\"1.2.3\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);

    // 굽는 중 같은 job 의 APPLY 재전송 → 무시
    m_nrep = 0;
    m_on_raw = dup_apply_hook; m_q_in_hook = -1;
    CHECK(server_apply(70));
    CHECK(m_q_in_hook == 0 && s_cur_apply == 0);
    CHECK(count_rep("\"job\":70,\"stage\":\"done\"") == FINAL_REPORTS && !s_stage.magic[0]);
    CHECK(strcmp(s_dsp_ver, "1.2.3") == 0 && s_state == ST_IDLE);
    // done 뒤 중복 APPLY (스테이징 지워짐) → not_staged 가 아니라 done{ver} 다시 보고, 다시 굽지 않음
    m_nrep = 0;
    int raws = m_raw_begins;
    CHECK(server_apply(70));
    CHECK(m_raw_begins == raws && count_rep("not_staged") == 0 && count_rep("\"start\"") == 0);
    CHECK(count_rep("\"job\":70,\"stage\":\"done\",\"pct\":100,\"ver\":\"1.2.3\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    // done 뒤 늦게 처리되는 중복 START → 다시 받지 않고 done 다시 보고
    m_nrep = 0;
    CHECK(server_start(70, 0));
    CHECK(m_http_inits == inits + 1 && count_rep("\"job\":70,\"stage\":\"done\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    // 다른 job 은 지금처럼 (스테이징 없음 → not_staged)
    m_nrep = 0;
    CHECK(server_apply(71));
    CHECK(count_rep("\"job\":0,\"stage\":\"fail\"") == FINAL_REPORTS && count_rep("not_staged") == FINAL_REPORTS);
    // 기억 기간(10분)이 지난 뒤의 같은 job → 지금처럼 처리
    m_now += (int64_t)(RECENT_S + 1) * 1000000;
    m_nrep = 0;
    CHECK(server_apply(70));
    CHECK(count_rep("not_staged") == FINAL_REPORTS && count_rep("\"done\"") == 0);

    // fail 뒤 중복 APPLY → 다시 굽지 않고 같은 fail 보고 (그 사이 앱이 정지 상태가 돼도)
    reset_board(APP_NG, "1.0");
    stage_raw(n, 72);
    s_state = ST_STAGED;
    CHECK(server_apply(72));
    CHECK(count_rep("\"job\":72,\"stage\":\"fail\",\"ver\":\"1.2.3\",\"err\":\"app_ng_01\"") == FINAL_REPORTS);
    m_app = APP_OK; m_nrep = 0;
    CHECK(server_apply(72));
    CHECK(count_rep("\"enter\"") == 0 && count_rep("\"start\"") == 0 && s_flash_untouched());
    CHECK(count_rep("\"job\":72,\"stage\":\"fail\",\"ver\":\"1.2.3\",\"err\":\"app_ng_01\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    // 그 상태의 중복 START → 스테이징이 그대로 있으므로 staged 다시 보고 (받지 않음)
    m_nrep = 0; inits = m_http_inits;
    CHECK(server_start(72, 0));
    CHECK(m_http_inits == inits && count_rep("\"job\":72,\"stage\":\"staged\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    // 다른 job 의 APPLY → 지금처럼 job_mismatch
    m_nrep = 0;
    CHECK(server_apply(73));
    CHECK(count_rep("\"job\":73,\"stage\":\"fail\",\"ver\":\"1.2.3\",\"err\":\"job_mismatch\"") == FINAL_REPORTS);

    // 받기 실패 뒤 중복 START → 다시 받지 않고 같은 fail 보고, 기존 스테이징(job 72) 유지
    uint32_t bad = mkimg("2026.10.11", NULL, 3000, 128);
    m_http_body = m_img; m_http_len = bad;
    m_nrep = 0; inits = m_http_inits;
    CHECK(server_start(74, 0));
    CHECK(m_http_inits == inits + 1 && count_rep("\"job\":74,\"stage\":\"fail\",\"err\":\"ver_invalid\"") == FINAL_REPORTS);
    m_nrep = 0;
    CHECK(server_start(74, 0));
    CHECK(m_http_inits == inits + 1 && count_rep("\"job\":74,\"stage\":\"fail\",\"err\":\"ver_invalid\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    CHECK(s_stage.magic[0] && s_stage.job == 72 && s_state == ST_STAGED);

    // START apply:1 — 받는 중 재전송 무시, 완료(done) 뒤 중복 → done 다시 보고 (받지도 굽지도 않음)
    n = mkimg("1.2.4", NULL, 3000, 128);
    m_http_body = m_img; m_http_len = n;
    reset_board(APP_OK, "1.2.3");
    m_on_raw = dup_start_hook; m_q_in_hook = -1;      // 이어지는 적용 중에도 같은 job 의 START 는 무시
    inits = m_http_inits;
    CHECK(server_start(75, 1));
    CHECK(m_q_in_hook == 0 && count_rep("\"job\":75,\"stage\":\"done\"") == FINAL_REPORTS && strcmp(s_dsp_ver, "1.2.4") == 0);
    m_nrep = 0; raws = m_raw_begins;
    CHECK(server_start(75, 1));
    CHECK(m_http_inits == inits + 1 && m_raw_begins == raws);
    CHECK(count_rep("\"job\":75,\"stage\":\"done\",\"pct\":100,\"ver\":\"1.2.4\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    m_http_body = NULL; m_http_len = 0;
}

static void t_json_safe(void)
{
    printf("-- 보고·상태 JSON 이스케이프\n");
    char b[300];
    m_nrep = 0;
    strlcpy(s_dsp_ver, "1\"2\\3", sizeof(s_dsp_ver));
    dsp_ota_status_json(b, sizeof(b));
    CHECK(strstr(b, "\"dsp\":\"1\\\"2\\\\3\"") != NULL);
    report_final(60, "done", 100, s_dsp_ver, NULL);
    CHECK(m_nrep == FINAL_REPORTS && strstr(m_rep[0], "\"ver\":\"1\\\"2\\\\3\"") != NULL);
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1;
    g_cfg.agv_id = 7;
    const uint8_t ip[4] = { 192, 168, 0, 10 };
    memcpy(&g_cfg.server_ip, ip, 4);
    memset(m_part, 0xFF, sizeof(m_part));
    CHECK(dsp_ota_init() == ESP_OK);
    t_download();
    t_apply_basic();
    t_apply_validate();
    t_retry();
    t_wait_power();
    t_alive_check();
    t_start_cmd();
    t_duplicate();
    t_json_safe();
    printf(s_fail ? "%d FAILED\n" : "flow: all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
