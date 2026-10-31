// main/dsp_ota.c 흐름 시험 (호스트): dsp_ota.c 를 그대로 포함하고 ESP-IDF·UART·HTTP·파티션을 가짜로 바꾼다.
// 메인보드 쪽은 실제 부트로더 프로토콜 처리기(dsp/common/bootproto.c)와 RAM 플래시, 간단한 앱 응답기로 흉내 낸다.
// 패키지 보관(dsp_pkg.c)은 그대로 링크하고 dsppkg 파티션(12MB RAM, NOR 처럼 쓰기는 1→0 만)·SHA-256(sha256_host.c)을 붙인다.
//   ./run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/dsp_ota.c"
#include "psa/crypto.h"

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

// ───────── 스테이징·패키지 파티션 (RAM) ─────────

#define PART_SIZE 0x60000
#define PKG_PART_SIZE 0xC00000
static uint8_t m_part[PART_SIZE];
static uint8_t m_pkgp[PKG_PART_SIZE];
static esp_partition_t m_p = { PART_SIZE, "dspfw" };
static esp_partition_t m_pp = { PKG_PART_SIZE, "dsppkg" };
static bool m_pkg_missing;          // 이전 파티션 표 (dsppkg 없음)
static int m_pkg_erases;            // dsppkg 지우기 횟수 (4KB 단위인지)
const esp_partition_t *esp_partition_find_first(int t, int st, const char *l)
{ return strcmp(l, "dsppkg") == 0 ? (m_pkg_missing ? NULL : &m_pp) : &m_p; }
static uint8_t *pmem(const esp_partition_t *p) { return p == &m_pp ? m_pkgp : m_part; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *d, size_t n)
{ if (off + n > p->size) return ESP_FAIL; memcpy(d, pmem(p) + off, n); return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *s, size_t n)
{
    if (off + n > p->size) return ESP_FAIL;
    for (size_t i = 0; i < n; i++) pmem(p)[off + i] &= ((const uint8_t *)s)[i];     // NOR: 1→0 만
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{
    if (off + n > p->size || off % 4096 || n % 4096) return ESP_FAIL;
    if (p == &m_pp) { m_pkg_erases++; if (n != 4096) return ESP_FAIL; }      // 한 번에 4KB 만 (캐시 정지 시간)
    memset(pmem(p) + off, 0xFF, n);
    return ESP_OK;
}

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

static const uint8_t *m_http_body;     // .dspfw
static uint32_t m_http_len, m_http_pos;
static const uint8_t *m_pkg_body;      // .dsppkg (URL 에 .dsppkg 가 들어간 요청)
static uint32_t m_pkg_len;
static int64_t m_pkg_clen = -1;        // >= 0 이면 패키지 Content-Length 를 이 값으로 (크기 불일치 흉내)
static const uint8_t *m_cur_body;
static uint32_t m_cur_len;
static bool m_cur_pkg;
static char m_http_url[256];
static const char *m_redirect_to;   // 첫 요청을 302 로 이 URL 에 보냄
static bool m_redirected;
static int m_http_inits;            // .dspfw 요청 수
static int m_pkg_inits;             // 패키지 요청 수
static void (*m_on_http_read)(void);    // HTTP 읽기 때 1회 불림 (받는 중 서버 재전송 흉내)
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    m_cur_pkg = strstr(c->url, ".dsppkg") != NULL;
    if (m_cur_pkg) { m_pkg_inits++; m_cur_body = m_pkg_body; m_cur_len = m_pkg_len; }
    else { m_http_inits++; m_cur_body = m_http_body; m_cur_len = m_http_len; }
    strncpy(m_http_url, c->url, sizeof(m_http_url) - 1); m_http_pos = 0; m_redirected = false; return (esp_http_client_handle_t)1;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int w) { m_http_pos = 0; return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{ return m_redirect_to && !m_redirected ? 0 : (m_cur_pkg && m_pkg_clen >= 0 ? m_pkg_clen : m_cur_len); }
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
    uint32_t n = m_cur_len - m_http_pos;
    if (n > 1000) n = 1000;             // 작은 조각으로 (머리 검사가 여러 조각을 모으는지)
    if ((uint32_t)len < n) n = (uint32_t)len;
    memcpy(b, m_cur_body + m_http_pos, n);
    m_http_pos += n;
    return (int)n;
}

// ───────── .dspfw 생성 (dsp_pack.py pack 과 같은 배치) ─────────

static uint8_t m_img[200000];
static void p16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
#define p32_ p32

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

// ───────── 패키지 (.dsppkg 바이트 — 브리지는 풀지 않으므로 내용은 아무 바이트) ─────────

extern int g_psa_open;
static uint8_t m_pkg[40000];
static char m_pkg_sha_hex[65], m_pkg_id[17];
static uint8_t m_pkg_sha[32];

static void sha256(const uint8_t *d, size_t n, uint8_t out[32])
{
    psa_hash_operation_t op = psa_hash_operation_init();
    size_t l;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, d, n);
    psa_hash_finish(&op, out, 32, &l);
}

// 패키지 바이트·sha256·ID 를 seed 로 만든다
static uint32_t mkpkg(uint32_t len, int seed)
{
    for (uint32_t i = 0; i < len; i++) m_pkg[i] = (uint8_t)(i * 31 + seed * 7 + (i >> 9));
    sha256(m_pkg, len, m_pkg_sha);
    for (int i = 0; i < 32; i++) sprintf(m_pkg_sha_hex + i * 2, "%02x", m_pkg_sha[i]);
    snprintf(m_pkg_id, sizeof(m_pkg_id), "%.16s", m_pkg_sha_hex + 20);
    m_pkg_body = m_pkg; m_pkg_len = len;
    return len;
}

// 실제 기록 경로(dsp_pkg_begin…finalize)로 짝 패키지를 넣는다
static void pkg_put(uint32_t job, uint32_t dspfw_crc, const char *ver)
{
    static uint8_t tmp[1000];
    if (!m_pkg_len) mkpkg(5000, 1);
    const char *e = dsp_pkg_begin(m_pkg_len, job, m_pkg_id, m_pkg_sha);
    for (uint32_t o = 0; !e && o < m_pkg_len; o += sizeof(tmp)) {
        uint32_t n = m_pkg_len - o < sizeof(tmp) ? m_pkg_len - o : (uint32_t)sizeof(tmp);
        memcpy(tmp, m_pkg + o, n);
        e = dsp_pkg_write(tmp, n);
    }
    if (!e) e = dsp_pkg_end();
    if (!e) e = dsp_pkg_finalize(dspfw_crc, ver);
    if (e) { printf("pkg_put: %s\n", e); s_fail++; }
}

// 규격 배치(LE 128 B 머리, 오프셋은 규격 표 그대로)로 슬롯 머리를 직접 쓴다 — USB 도구가 쓰는 슬롯 흉내
static void pkg_hdr_raw(int slot, uint32_t flags, uint32_t seq, uint32_t job, uint32_t crc, const char *id, const char *ver)
{
    uint8_t *b = m_pkgp + (size_t)slot * 0x600000;
    memset(b, 0xFF, 4096);
    memcpy(b, "DSPPKG1", 8);
    p32_(b + 8, flags); p32_(b + 12, seq); p32_(b + 16, m_pkg_len); p32_(b + 20, job); p32_(b + 24, crc);
    memset(b + 28, 0, 17); memcpy(b + 28, id, 16);
    memset(b + 45, 0, 16); memcpy(b + 45, ver, strlen(ver));
    memcpy(b + 61, m_pkg_sha, 32);
    memset(b + 0x1000, 0xFF, m_pkg_len);
    memcpy(b + 0x1000, m_pkg, m_pkg_len);
}
static uint32_t pkg_flags(int slot) { return get32(m_pkgp + (size_t)slot * 0x600000 + 8); }

static uint32_t img_crc(void) { return get32(m_img + 12); }

// 다운로드 없이 파티션에 직접 스테이징 (USB 도구·구 펌웨어가 받아 둔 이미지 흉내). pkg = 짝 패키지도 넣음
static void stage_raw_ex(uint32_t len, uint32_t job, bool pkg)
{
    memset(m_part, 0xFF, sizeof(m_part));
    memcpy(m_part + STAGE_HDR_LEN, m_img, len);
    stage_hdr_t sh = { .job = job, .file_len = len, .file_crc = img_crc() };
    memcpy(sh.magic, STAGE_MAGIC, 8);
    memcpy(sh.ver, m_img + 16, 15);
    memcpy(m_part, &sh, sizeof(sh));
    stage_load();
    if (pkg && dap_ver_valid(s_stage.ver)) pkg_put(job, img_crc(), s_stage.ver);    // 결함 이미지 시험은 패키지 없이
}
static void stage_raw(uint32_t len, uint32_t job) { stage_raw_ex(len, job, true); }

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
    s_dl.base = 0; s_dl.last_pct = -1;
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

#define PKG_URL "http://192.168.0.10:8080/fw/20261011-000000-abcdef/app.dsppkg"
static char m_pkgjs[400];
// START 의 pkg_* 필드 (",..." 형태, 앞에 쉼표)
static const char *pkg_fields(void)
{
    snprintf(m_pkgjs, sizeof(m_pkgjs), ",\"pkg_url\":\"%s\",\"pkg_size\":%u,\"pkg_sha256\":\"%s\",\"pkg_id\":\"%s\"",
             PKG_URL, m_pkg_len, m_pkg_sha_hex, m_pkg_id);
    return m_pkgjs;
}

static void t_start_cmd(void)
{
    printf("-- DSP_OTA_START 검사 (C-09 / 결정 11)\n");
    char js[800], url[300];
    memset(url, 'a', sizeof(url));
    memcpy(url, "http://192.168.0.10:8080/fw/", 28);
    url[250] = 0;
    snprintf(js, sizeof(js), "{\"job\":50,\"url\":\"%s\",\"crc32\":1}", url);
    m_nrep = 0; m_queued = 0;
    dsp_ota_start(js, strlen(js));
    CHECK(m_queued == 0 && count_rep("\"err\":\"url_too_long\"") == FINAL_REPORTS && count_rep("\"job\":50") == FINAL_REPORTS);
    url[199] = 0;                                   // 정확히 199자 → 통과
    snprintf(js, sizeof(js), "{\"job\":51,\"url\":\"%s\",\"recover\":1%s}", url, pkg_fields());
    dsp_ota_start(js, strlen(js));
    CHECK(m_queued == 1 && strcmp(m_last_cmd.url, url) == 0 && m_last_cmd.recover);
    CHECK(strcmp(m_last_cmd.pkg_url, PKG_URL) == 0 && m_last_cmd.pkg_size == m_pkg_len && strcmp(m_last_cmd.pkg_id, m_pkg_id) == 0 &&
          memcmp(m_last_cmd.pkg_sha, m_pkg_sha, 32) == 0);
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

static char m_js_start[800], m_js_apply[100];
static int m_q_in_hook;
static void dup_start_hook(void) { int q = m_queued; dsp_ota_start(m_js_start, strlen(m_js_start)); m_q_in_hook = m_queued - q; }
static void dup_apply_hook(void) { int q = m_queued; dsp_ota_apply(m_js_apply, strlen(m_js_apply)); m_q_in_hook = m_queued - q; }

// 서버 명령 → 큐에 들어가면 처리. 큐에 들어갔으면 true
static bool server_start(uint32_t job, int apply)
{
    snprintf(m_js_start, sizeof(m_js_start), "{\"job\":%u,\"url\":\"%s\",\"size\":%u,\"apply\":%d%s}", job, URL, m_http_len, apply, pkg_fields());
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

// ───────── 패키지 보관 (dsppkg, 0.9.0) ─────────

static int pkg_slot_of(const char *id)
{
    for (int i = 0; i < 2; i++) if (dsp_pkg_slot(i)->st != DSP_PKG_EMPTY && strcmp(dsp_pkg_slot(i)->id, id) == 0) return i;
    return -1;
}

static void pkg_wipe(void)
{
    memset(m_pkgp, 0xFF, sizeof(m_pkgp));
    m_pkg_missing = false;
    CHECK(dsp_pkg_init());
}

// download 보고의 pct 가 늘기만 하고 100 으로 끝나는지, 앞쪽(패키지 단계)에도 보고가 있는지
static bool pct_monotonic(uint32_t job, int *count, int *first)
{
    char key[48];
    int last = -1, c = 0;
    snprintf(key, sizeof(key), "\"job\":%u,\"stage\":\"download\",\"pct\":", job);
    for (int i = 0; i < m_nrep; i++) {
        const char *p = strstr(m_rep[i], key);
        if (!p) continue;
        int v = atoi(p + strlen(key));
        if (c == 0 && first) *first = v;
        if (v <= last) return false;
        last = v; c++;
    }
    if (count) *count = c;
    return last == 100;
}

static void t_pkg_start(void)
{
    printf("-- 패키지: START 검사·받기 (no_pkg / pkg_sha / pkg_size / 짝 확정 / pct)\n");
    uint32_t n = mkimg("1.3.0", NULL, 3000, 128);
    reset_board(APP_OK, "1.2.4");
    pkg_wipe();
    mkpkg(5000, 2);
    stage_raw(n, 90);                                   // 기존 스테이징 + 짝 패키지
    s_state = ST_STAGED;
    memset(&s_recent, 0, sizeof(s_recent));
    static uint8_t snap[PART_SIZE];
    memcpy(snap, m_part, sizeof(snap));
    int pi = m_pkg_inits, hi = m_http_inits;
    char js[900];

    // 1. pkg_* 없음 / 형식 오류 → no_pkg, 큐에 넣지 않음, 기존 스테이징·패키지 그대로
    const char *bad[] = {
        "",                                                                                   // 0.6.x 게이트웨이
        ",\"pkg_url\":\"" PKG_URL "\",\"pkg_size\":5000,\"pkg_id\":\"0123456789abcdef\"",      // sha 없음
        ",\"pkg_url\":\"" PKG_URL "\",\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789ABCDEF\"",   // 대문자 ID
        ",\"pkg_url\":\"" PKG_URL "\",\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcde\"",    // 15자
        ",\"pkg_url\":\"" PKG_URL "\",\"pkg_size\":0,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcdef\"",      // 크기 0
        ",\"pkg_url\":\"ftp://x/a.dsppkg\",\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcdef\"",
        ",\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcdef\"",                                 // url 없음
    };
    char upper[65];
    for (size_t k = 0; k <= sizeof(bad) / sizeof(bad[0]); k++) {
        char f[400];
        if (k < sizeof(bad) / sizeof(bad[0])) {
            snprintf(f, sizeof(f), bad[k], m_pkg_sha_hex);
        } else {                                        // sha 대문자
            for (int i = 0; i < 64; i++) upper[i] = (char)(m_pkg_sha_hex[i] >= 'a' ? m_pkg_sha_hex[i] - 32 : m_pkg_sha_hex[i]);
            upper[64] = 0;
            snprintf(f, sizeof(f), ",\"pkg_url\":\"" PKG_URL "\",\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"%s\"", upper, m_pkg_id);
        }
        snprintf(js, sizeof(js), "{\"job\":91,\"url\":\"%s\"%s}", URL, f);
        m_nrep = 0; m_queued = 0;
        dsp_ota_start(js, strlen(js));
        CHECK(m_queued == 0 && count_rep("\"job\":91,\"stage\":\"fail\",\"err\":\"no_pkg\"") == FINAL_REPORTS && m_nrep == FINAL_REPORTS);
    }
    // 크기 초과 · URL 호스트 · URL 길이
    struct { const char *f; const char *err; } cases[] = {
        { ",\"pkg_url\":\"" PKG_URL "\",\"pkg_size\":6287361,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcdef\"", "pkg_too_big" },
        { ",\"pkg_url\":\"http://10.0.0.1/a.dsppkg\",\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcdef\"", "pkg_url_host" },
        { ",\"pkg_url\":\"http://192.168.0.10/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.dsppkg\","
          "\"pkg_size\":5000,\"pkg_sha256\":\"%s\",\"pkg_id\":\"0123456789abcdef\"", "pkg_url_too_long" },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        char f[500], want[80];
        snprintf(f, sizeof(f), cases[k].f, m_pkg_sha_hex);
        snprintf(js, sizeof(js), "{\"job\":92,\"url\":\"%s\"%s}", URL, f);
        snprintf(want, sizeof(want), "\"err\":\"%s\"", cases[k].err);
        m_nrep = 0; m_queued = 0;
        dsp_ota_start(js, strlen(js));
        CHECK(m_queued == 0 && count_rep(want) == FINAL_REPORTS);
    }
    CHECK(memcmp(snap, m_part, sizeof(snap)) == 0 && s_stage.job == 90 && m_pkg_inits == pi && m_http_inits == hi);
    CHECK(dsp_pkg_find_staged(img_crc()) >= 0);

    // 2. 패키지 해시 불일치 → pkg_sha: .dspfw 는 받지 않음, 기존 스테이징 그대로, 기록하던 슬롯은 빈 슬롯
    n = mkimg("1.3.1", NULL, 3000, 128);
    m_http_body = m_img; m_http_len = n;
    mkpkg(40000, 3);
    m_pkg[12345] ^= 0x40;                               // 서버가 알려준 sha256 과 다른 내용
    m_nrep = 0;
    CHECK(server_start(93, 0));
    CHECK(count_rep("\"job\":93,\"stage\":\"fail\",\"err\":\"pkg_sha\"") == FINAL_REPORTS);
    CHECK(m_pkg_inits == pi + 1 && m_http_inits == hi && s_stage.job == 90 && s_state == ST_STAGED);
    CHECK(memcmp(snap, m_part, sizeof(snap)) == 0 && g_psa_open == 0);
    CHECK(pkg_slot_of(m_pkg_id) < 0);
    // 크기 불일치 (Content-Length ≠ pkg_size) → pkg_size
    mkpkg(40000, 3);
    m_pkg_clen = 39999; m_nrep = 0;
    CHECK(server_start(94, 0));
    m_pkg_clen = -1;
    CHECK(count_rep("\"job\":94,\"stage\":\"fail\",\"err\":\"pkg_size\"") == FINAL_REPORTS && s_stage.job == 90 && g_psa_open == 0);
    // .dspfw 받기 실패 (패키지는 받음) → 패키지 슬롯 확정 안 됨 (bit0=1 → 무시)
    uint32_t badn = mkimg("2026.10.11", NULL, 3000, 128);
    m_http_body = m_img; m_http_len = badn; m_nrep = 0;
    CHECK(server_start(95, 0));
    CHECK(count_rep("\"job\":95,\"stage\":\"fail\",\"err\":\"ver_invalid\"") == FINAL_REPORTS && pkg_slot_of(m_pkg_id) < 0);

    // 3. 정상: 패키지 → .dspfw, 슬롯 STAGED + 짝(.dspfw 파일 CRC), pct 는 합계 기준으로 늘기만
    n = mkimg("1.3.1", NULL, 3000, 128);
    m_http_body = m_img; m_http_len = n;
    m_nrep = 0; m_pkg_erases = 0;
    CHECK(server_start(96, 0));
    CHECK(count_rep("\"job\":96,\"stage\":\"staged\",\"pct\":100,\"ver\":\"1.3.1\"") == FINAL_REPORTS);
    int cnt = 0, first = -1;
    CHECK(pct_monotonic(96, &cnt, &first) && cnt >= 4 && first < 100);    // 패키지(40000 B) 단계에서 20·40·60·80
    int sl = pkg_slot_of(m_pkg_id);
    CHECK(sl >= 0 && dsp_pkg_slot(sl)->st == DSP_PKG_STAGED && dsp_pkg_slot(sl)->dspfw_crc == s_stage.file_crc &&
          s_stage.file_crc == img_crc() && dsp_pkg_slot(sl)->job == 96 && strcmp(dsp_pkg_slot(sl)->ver, "1.3.1") == 0);
    CHECK(m_pkg_erases == 1 + (40000 + 4095) / 4096);  // 머리 섹터 + 패키지 섹터, 4KB 씩
    const uint8_t *h = m_pkgp + (size_t)sl * 0x600000;   // 규격 배치 그대로
    CHECK(memcmp(h, "DSPPKG1\0", 8) == 0 && get32(h + 8) == 0xFFFFFFFE && get32(h + 16) == 40000 && get32(h + 20) == 96 &&
          get32(h + 24) == img_crc() && memcmp(h + 28, m_pkg_id, 17) == 0 && strcmp((const char *)h + 45, "1.3.1") == 0 &&
          memcmp(h + 61, m_pkg_sha, 32) == 0 && h[93] == 0xFF && h[127] == 0xFF && h[128] == 0xFF);
    CHECK(memcmp(h + 0x1000, m_pkg, 40000) == 0 && g_psa_open == 0);
    char b[600], want[120];
    dsp_ota_status_json(b, sizeof(b));
    snprintf(want, sizeof(want), "\"staged\":{\"job\":96,\"ver\":\"1.3.1\",\"pkg\":\"%s\"},\"pkg_st\":\"ok\"", m_pkg_id);
    CHECK(strstr(b, want) != NULL);
    dsp_ota_format(b, sizeof(b));
    snprintf(want, sizeof(want), "staged_pkg=%s\n", m_pkg_id);
    CHECK(strstr(b, want) != NULL);
    // 같은 job 의 중복 START → 패키지도 다시 받지 않음
    pi = m_pkg_inits; m_nrep = 0;
    CHECK(server_start(96, 0));
    CHECK(m_pkg_inits == pi && count_rep("\"job\":96,\"stage\":\"staged\"") == FINAL_REPORTS);
}

static void t_pkg_apply(void)
{
    printf("-- 패키지: 적용 전 짝 확인 / APPLIED·OLD / 전원 차단 뒤 seq\n");
    uint32_t n = mkimg("1.4.0", NULL, 3000, 128);
    char b[600], want[120];

    // 1. 짝 패키지 없음 → no_pkg, 메인보드 그대로
    reset_board(APP_OK, "1.3.1");
    pkg_wipe();
    stage_raw_ex(n, 100, false);
    const char *e = apply(100, false);
    CHECK(e && strcmp(e, "no_pkg") == 0 && m_raw_begins == 0 && s_flash_untouched() && s_stage.magic[0]);
    m_nrep = 0;
    CHECK(server_apply(100));
    CHECK(count_rep("\"job\":100,\"stage\":\"fail\",\"ver\":\"1.4.0\",\"err\":\"no_pkg\"") == FINAL_REPORTS && s_flash_untouched());
    //    다른 이미지의 패키지만 있음 (dspfw_crc 다름) → no_pkg
    mkpkg(3000, 4);
    pkg_put(100, img_crc() ^ 1, "1.4.0");
    reset_board(APP_OK, "1.3.1");
    e = apply(100, false);
    CHECK(e && strcmp(e, "no_pkg") == 0 && m_raw_begins == 0 && s_flash_untouched());
    //    짝은 있으나 flash 의 패키지 바이트가 손상 → no_pkg (sha256 재계산)
    pkg_wipe();
    stage_raw(n, 101);
    int sl = dsp_pkg_find_staged(img_crc());
    CHECK(sl >= 0);
    uint8_t *pb = m_pkgp + (size_t)sl * 0x600000 + 0x1000 + 777;
    while (!(*pb & 0x80)) pb++;
    *pb &= 0x7F;                                        // NOR 처럼 1→0 (실제로 바뀌는 바이트)
    reset_board(APP_OK, "1.3.1");
    e = apply(101, false);
    CHECK(e && strcmp(e, "no_pkg") == 0 && m_raw_begins == 0 && s_flash_untouched() && g_psa_open == 0);
    CHECK(dsp_pkg_slot(sl)->st == DSP_PKG_STAGED);

    // 2. 적용 성공 → APPLIED. 다음 적용 → 새 슬롯 APPLIED, 이전 슬롯 OLD
    pkg_wipe();
    mkpkg(3000, 5);
    char id_a[17];
    strcpy(id_a, m_pkg_id);
    stage_raw(n, 102);
    int a = pkg_slot_of(id_a);
    reset_board(APP_OK, "1.3.1");
    CHECK(apply(102, false) == NULL && strcmp(s_dsp_ver, "1.4.0") == 0);
    CHECK(a == 0 && dsp_pkg_slot(a)->st == DSP_PKG_APPLIED && dsp_pkg_current() == a && pkg_flags(a) == 0xFFFFFFFC);
    dsp_ota_status_json(b, sizeof(b));
    snprintf(want, sizeof(want), "\"staged\":null,\"pkg_st\":\"ok\",\"dsp_pkg\":{\"id\":\"%s\",\"ver\":\"1.4.0\"}", id_a);
    CHECK(strstr(b, want) != NULL);
    dsp_ota_format_json(b, sizeof(b));
    snprintf(want, sizeof(want), "\"pkg_st\":\"ok\",\"pkg\":{\"id\":\"%s\",\"ver\":\"1.4.0\"}}", id_a);
    CHECK(strstr(b, want) != NULL);
    dsp_ota_format(b, sizeof(b));
    snprintf(want, sizeof(want), "pkg=%s(1.4.0) staged_pkg=-\n", id_a);
    CHECK(strstr(b, want) != NULL);

    n = mkimg("1.4.1", NULL, 3000, 128);
    mkpkg(4000, 6);
    char id_b[17];
    strcpy(id_b, m_pkg_id);
    stage_raw(n, 103);                                  // APPLIED 가 아닌 슬롯 1 로
    CHECK(pkg_slot_of(id_b) == 1 && dsp_pkg_slot(1)->seq == dsp_pkg_slot(0)->seq + 1);
    dsp_ota_pkg_format(b, sizeof(b));
    CHECK(strstr(b, "pkg slot0: APPLIED") && strstr(b, "pkg slot1: STAGED") && strstr(b, "<- staged") && strstr(b, "job=103"));
    reset_board(APP_OK, "1.4.0");
    CHECK(apply(103, false) == NULL && strcmp(s_dsp_ver, "1.4.1") == 0);
    CHECK(dsp_pkg_current() == 1 && dsp_pkg_slot(0)->st == DSP_PKG_OLD && pkg_flags(0) == 0xFFFFFFF8 && pkg_flags(1) == 0xFFFFFFFC);
    //    적용 실패 → 슬롯 STAGED 그대로
    n = mkimg("1.4.2", NULL, 3000, 128);
    mkpkg(3500, 7);
    stage_raw(n, 104);
    CHECK(pkg_slot_of(m_pkg_id) == 0);                  // 현재 APPLIED(1) 가 아닌 쪽 (OLD 를 덮어씀)
    reset_board(APP_NG, "1.4.1");
    e = apply(104, false);
    CHECK(e && strcmp(e, "app_ng_01") == 0 && dsp_pkg_slot(0)->st == DSP_PKG_STAGED && dsp_pkg_current() == 1);
    //    START apply:1 로 받고 바로 적용
    n = mkimg("1.4.3", NULL, 3000, 128);
    m_http_body = m_img; m_http_len = n;
    mkpkg(9000, 8);
    reset_board(APP_OK, "1.4.1");
    memset(&s_recent, 0, sizeof(s_recent));
    CHECK(server_start(105, 1));
    CHECK(count_rep("\"job\":105,\"stage\":\"done\",\"pct\":100,\"ver\":\"1.4.3\"") == FINAL_REPORTS);
    CHECK(dsp_pkg_current() == 0 && strcmp(dsp_pkg_slot(0)->id, m_pkg_id) == 0 && dsp_pkg_slot(1)->st == DSP_PKG_OLD);

    // 3. 전원 차단으로 두 슬롯이 모두 APPLIED → seq 큰 쪽이 현재, 새 패키지는 작은 쪽에, 적용하면 큰 쪽을 OLD 로 표시
    pkg_wipe();
    mkpkg(2000, 9);
    pkg_hdr_raw(0, 0xFFFFFFFC, 6, 7, 0x1111, "aaaaaaaaaaaaaaaa", "2.0");
    pkg_hdr_raw(1, 0xFFFFFFFC, 5, 8, 0x2222, "bbbbbbbbbbbbbbbb", "1.9");
    CHECK(dsp_pkg_init());
    CHECK(dsp_pkg_current() == 0 && dsp_pkg_slot(1)->st == DSP_PKG_OLD);
    pkg_hdr_raw(0, 0xFFFFFFFC, 5, 7, 0x1111, "aaaaaaaaaaaaaaaa", "2.0");
    pkg_hdr_raw(1, 0xFFFFFFFC, 9, 8, 0x2222, "bbbbbbbbbbbbbbbb", "1.9");
    CHECK(dsp_pkg_init());
    CHECK(dsp_pkg_current() == 1 && dsp_pkg_slot(0)->st == DSP_PKG_OLD);
    dsp_ota_status_json(b, sizeof(b));
    CHECK(strstr(b, "\"dsp_pkg\":{\"id\":\"bbbbbbbbbbbbbbbb\",\"ver\":\"1.9\"}") != NULL);
    n = mkimg("2.1", NULL, 3000, 128);
    mkpkg(2500, 10);
    stage_raw(n, 106);
    CHECK(pkg_slot_of(m_pkg_id) == 0 && dsp_pkg_slot(0)->seq == 10);
    reset_board(APP_OK, "1.9");
    CHECK(apply(106, false) == NULL);
    CHECK(dsp_pkg_current() == 0 && pkg_flags(1) == 0xFFFFFFF8 && dsp_pkg_slot(1)->st == DSP_PKG_OLD);
    //    쓰다 만 슬롯 (bit0=1) 은 빈 슬롯
    pkg_hdr_raw(1, 0xFFFFFFFF, 11, 0, 0x3333, "cccccccccccccccc", "3.0");
    CHECK(dsp_pkg_init() && dsp_pkg_slot(1)->st == DSP_PKG_EMPTY && dsp_pkg_current() == 0);
    dsp_ota_pkg_format(b, sizeof(b));
    CHECK(strstr(b, "pkg slot1: empty") != NULL);

    // 4. dsppkg 파티션 없음 (이전 파티션 표 + OTA 로 받은 0.9.0) → pkg_nopart
    m_pkg_missing = true;
    CHECK(!dsp_pkg_init());
    mkpkg(2000, 11);
    snprintf(m_js_start, sizeof(m_js_start), "{\"job\":107,\"url\":\"%s\"%s}", URL, pkg_fields());
    m_nrep = 0; m_queued = 0;
    dsp_ota_start(m_js_start, strlen(m_js_start));
    CHECK(m_queued == 0 && count_rep("\"job\":107,\"stage\":\"fail\",\"err\":\"pkg_nopart\"") == FINAL_REPORTS);
    n = mkimg("2.2", NULL, 3000, 128);
    reset_board(APP_OK, "2.1");
    stage_raw_ex(n, 108, false);
    e = apply(108, false);
    CHECK(e && strcmp(e, "pkg_nopart") == 0 && m_raw_begins == 0 && s_flash_untouched());
    dsp_ota_status_json(b, sizeof(b));
    CHECK(strstr(b, "\"pkg_st\":\"nopart\",\"dsp_pkg\":null") != NULL && strstr(b, "\"staged\":{\"job\":108,\"ver\":\"2.2\"}") != NULL);
    dsp_ota_format(b, sizeof(b));
    CHECK(strstr(b, "pkg=nopart staged_pkg=-") != NULL);
    dsp_ota_pkg_format(b, sizeof(b));
    CHECK(strstr(b, "nopart") != NULL);
    pkg_wipe();
    stage_clear();
    s_state = ST_IDLE;
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
    memset(m_pkgp, 0xFF, sizeof(m_pkgp));
    mkpkg(5000, 1);
    CHECK(dsp_ota_init() == ESP_OK);
    t_download();
    t_apply_basic();
    t_apply_validate();
    t_retry();
    t_wait_power();
    t_alive_check();
    t_start_cmd();
    t_duplicate();
    t_pkg_start();
    t_pkg_apply();
    t_json_safe();
    printf(s_fail ? "%d FAILED\n" : "flow: all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
