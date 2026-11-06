#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include "dsp_app_proto.h"
#include "dsp_image.h"
#include "bridge_util.h"
#include "dsp_pkg.h"

static const char *TAG = "dsp_pkg";

#define HDR_MAGIC       "DSPPKG1"       // 8바이트 (NUL 포함)
#define HDR_LEN         128
#define F_WRITTEN       (1u << 0)       // 0 = 기록 완료 (패키지 바이트·sha256 확인 끝)
#define F_APPLIED       (1u << 1)       // 0 = 적용됨 (짝 .dspfw 를 굽고 버전 확인까지 끝남)
#define F_OLD           (1u << 2)       // 0 = 지난 것 (다른 슬롯이 나중에 적용됨)
#define SECTOR          4096
#define IO_BUF          4096
#define YIELD_BYTES     (64 * 1024)     // 다시 해시할 때 이만큼마다 양보 (태스크 워치독, 중계 태스크)

// 슬롯 머리 (LE, 128 B, 섹터 나머지는 0xFF)
typedef struct {
    char     magic[8];
    uint32_t flags;
    uint32_t seq;           // 기록할 때마다 두 슬롯의 최댓값 + 1
    uint32_t pkg_size;
    uint32_t job;           // 게이트웨이 작업 번호 (USB 는 0)
    uint32_t dspfw_crc;     // 짝 .dspfw 의 파일 CRC (= 스테이징 머리 file_crc)
    char     pkg_id[17];
    char     ver[16];
    uint8_t  sha256[32];
    uint8_t  rsvd[35];
} pkg_hdr_t;
_Static_assert(sizeof(pkg_hdr_t) == HDR_LEN, "pkg header size");
_Static_assert(offsetof(pkg_hdr_t, flags) == 8 && offsetof(pkg_hdr_t, pkg_id) == 28 &&
               offsetof(pkg_hdr_t, ver) == 45 && offsetof(pkg_hdr_t, sha256) == 61, "pkg header layout");

static const esp_partition_t *s_part;
static dsp_pkg_slot_t s_slot[DSP_PKG_SLOTS];
// 기록 중인 슬롯 (dsp 태스크 전용)
static struct {
    bool     active;        // begin ~ finalize/abort
    bool     hashing;       // 해시 연산 진행 중 (abort 필요)
    bool     done;          // end 통과 (크기·sha256 확인)
    int      slot;
    uint32_t size, pos, erased, job, seq;
    char     id[17];
    uint8_t  sha[32];
} s_w;
static psa_hash_operation_t s_op;
static uint8_t s_buf[IO_BUF];

static uint32_t slot_base(int i)
{
    return (uint32_t)i * DSP_PKG_SLOT_SIZE;
}

static bool hex_ok(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return s[n] == '\0';
}

static void load_slot(int i)
{
    pkg_hdr_t h;
    dsp_pkg_slot_t s = { .st = DSP_PKG_EMPTY, .flags = 0xFFFFFFFF };
    if (esp_partition_read(s_part, slot_base(i), &h, sizeof(h)) == ESP_OK && memcmp(h.magic, HDR_MAGIC, 8) == 0 &&
        !(h.flags & F_WRITTEN)) {
        h.pkg_id[16] = '\0';
        h.ver[15] = '\0';
        if (h.pkg_size >= 1 && h.pkg_size <= DSP_PKG_MAX && hex_ok(h.pkg_id, 16) && dap_ver_valid(h.ver)) {
            s.st    = (h.flags & F_APPLIED) ? DSP_PKG_STAGED : (h.flags & F_OLD) ? DSP_PKG_APPLIED : DSP_PKG_OLD;
            s.flags = h.flags;
            s.seq   = h.seq;
            s.size  = h.pkg_size;
            s.job   = h.job;
            s.dspfw_crc = h.dspfw_crc;
            memcpy(s.id, h.pkg_id, sizeof(s.id));
            memcpy(s.ver, h.ver, sizeof(s.ver));
            memcpy(s.sha, h.sha256, sizeof(s.sha));
        } else {
            ESP_LOGW(TAG, "slot %d: header fields invalid - ignored", i);
        }
    }
    s_slot[i] = s;
}

// 두 슬롯을 다시 읽고 해석: 둘 다 APPLIED(OLD 표시 전 전원 차단)면 seq 큰 쪽이 현재, 작은 쪽은 OLD
static void reload_all(void)
{
    for (int i = 0; i < DSP_PKG_SLOTS; i++) {
        load_slot(i);
    }
    if (s_slot[0].st == DSP_PKG_APPLIED && s_slot[1].st == DSP_PKG_APPLIED) {
        s_slot[s_slot[0].seq < s_slot[1].seq ? 0 : 1].st = DSP_PKG_OLD;
    }
}

bool dsp_pkg_init(void)
{
    (void)psa_crypto_init();                // 시작 때 이미 초기화됨 (여러 번 불러도 됨)
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, DSP_PKG_LABEL);
    if (s_part && s_part->size < DSP_PKG_SLOTS * DSP_PKG_SLOT_SIZE) {
        ESP_LOGE(TAG, "partition '%s' too small (0x%lx) - ignored", DSP_PKG_LABEL, (unsigned long)s_part->size);
        s_part = NULL;
    }
    if (!s_part) {
        ESP_LOGE(TAG, "partition '%s' not found - mainboard staging/apply refused (pkg_nopart) until this bridge is "
                 "reflashed over USB with the 0.9.0 partition table (16MB)", DSP_PKG_LABEL);
        return false;
    }
    reload_all();
    for (int i = 0; i < DSP_PKG_SLOTS; i++) {
        const dsp_pkg_slot_t *s = &s_slot[i];
        if (s->st != DSP_PKG_EMPTY) {
            ESP_LOGI(TAG, "slot %d: %s id %s ver %s (%lu B, seq %lu)", i, dsp_pkg_state_name(s->st), s->id, s->ver,
                     (unsigned long)s->size, (unsigned long)s->seq);
        }
    }
    return true;
}

bool dsp_pkg_ready(void)
{
    return s_part != NULL;
}

const dsp_pkg_slot_t *dsp_pkg_slot(int i)
{
    return &s_slot[i & 1];
}

int dsp_pkg_current(void)
{
    for (int i = 0; i < DSP_PKG_SLOTS; i++) {
        if (s_slot[i].st == DSP_PKG_APPLIED) {
            return i;
        }
    }
    return -1;
}

int dsp_pkg_find_staged(uint32_t dspfw_crc)
{
    int r = -1;
    for (int i = 0; i < DSP_PKG_SLOTS; i++) {
        if (s_slot[i].st == DSP_PKG_STAGED && s_slot[i].dspfw_crc == dspfw_crc && (r < 0 || s_slot[i].seq > s_slot[r].seq)) {
            r = i;
        }
    }
    return r;
}

const char *dsp_pkg_state_name(dsp_pkg_state_t st)
{
    switch (st) {
    case DSP_PKG_STAGED:  return "STAGED";
    case DSP_PKG_APPLIED: return "APPLIED";
    case DSP_PKG_OLD:     return "OLD";
    default:              return "empty";
    }
}

// ───────── 기록 ─────────

// 새 패키지를 쓸 슬롯: 현재 APPLIED 가 아닌 슬롯 (둘 다 아니면 빈 슬롯, 없으면 seq 가 작은 쪽)
static int choose_slot(void)
{
    int cur = dsp_pkg_current();
    if (cur >= 0) {
        return 1 - cur;
    }
    if (s_slot[0].st == DSP_PKG_EMPTY) {
        return 0;
    }
    if (s_slot[1].st == DSP_PKG_EMPTY) {
        return 1;
    }
    return s_slot[0].seq <= s_slot[1].seq ? 0 : 1;
}

void dsp_pkg_abort(void)
{
    if (s_w.hashing) {
        psa_hash_abort(&s_op);
    }
    bool was = s_w.active;
    memset(&s_w, 0, sizeof(s_w));
    if (was && s_part) {
        reload_all();                       // 머리를 지웠으므로 그 슬롯은 빈 슬롯
    }
}

const char *dsp_pkg_begin(uint32_t size, uint32_t job, const char *id, const uint8_t sha[32])
{
    dsp_pkg_abort();
    if (!s_part) {
        return "pkg_nopart";
    }
    if (size == 0 || size > DSP_PKG_MAX) {
        return "pkg_too_big";
    }
    uint32_t seq = 0;
    for (int i = 0; i < DSP_PKG_SLOTS; i++) {
        if (s_slot[i].st != DSP_PKG_EMPTY && s_slot[i].seq > seq) {
            seq = s_slot[i].seq;
        }
    }
    s_w.slot = choose_slot();
    s_w.size = size;
    s_w.job  = job;
    s_w.seq  = seq + 1;
    strlcpy(s_w.id, id, sizeof(s_w.id));
    memcpy(s_w.sha, sha, sizeof(s_w.sha));
    s_w.active = true;
    // 머리 섹터부터 지운다 → 이 슬롯은 기록이 끝날 때까지 빈 슬롯. 나머지는 쓰기 직전에 4KB 씩
    s_slot[s_w.slot] = (dsp_pkg_slot_t){ .st = DSP_PKG_EMPTY, .flags = 0xFFFFFFFF };
    if (esp_partition_erase_range(s_part, slot_base(s_w.slot), SECTOR) != ESP_OK) {
        return "pkg_flash";
    }
    s_w.erased = DSP_PKG_HDR_SECTOR;
    s_op = psa_hash_operation_init();
    if (psa_hash_setup(&s_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return "pkg_sha";
    }
    s_w.hashing = true;
    ESP_LOGI(TAG, "writing package %s (%lu B) to slot %d, seq %lu", s_w.id, (unsigned long)size, s_w.slot, (unsigned long)s_w.seq);
    return NULL;
}

const char *dsp_pkg_write(uint8_t *buf, size_t n)
{
    if (!s_w.hashing) {
        return "pkg_flash";
    }
    if ((uint64_t)s_w.pos + n > s_w.size) {
        return "pkg_size";
    }
    const uint32_t base = slot_base(s_w.slot);
    while (s_w.erased < DSP_PKG_HDR_SECTOR + s_w.pos + n) {
        // 4KB 씩: 크게 지우면 그동안 캐시가 꺼져 UART 중계 태스크까지 멈춘다
        if (esp_partition_erase_range(s_part, base + s_w.erased, SECTOR) != ESP_OK) {
            return "pkg_flash";
        }
        s_w.erased += SECTOR;
        vTaskDelay(1);
    }
    // 쓴 바이트를 flash 에서 다시 읽어 해시 → 받은 내용과 기록 내용을 한 번에 확인
    const uint32_t off = base + DSP_PKG_HDR_SECTOR + s_w.pos;
    if (esp_partition_write(s_part, off, buf, n) != ESP_OK || esp_partition_read(s_part, off, buf, n) != ESP_OK) {
        return "pkg_flash";
    }
    if (psa_hash_update(&s_op, buf, n) != PSA_SUCCESS) {
        return "pkg_sha";
    }
    s_w.pos += n;
    return NULL;
}

const char *dsp_pkg_end(void)
{
    uint8_t h[32];
    size_t hl = 0;
    if (!s_w.hashing) {
        return "pkg_flash";
    }
    if (s_w.pos != s_w.size) {
        return "pkg_size";
    }
    s_w.hashing = false;
    if (psa_hash_finish(&s_op, h, sizeof(h), &hl) != PSA_SUCCESS) {
        psa_hash_abort(&s_op);
        return "pkg_sha";
    }
    if (hl != sizeof(h) || memcmp(h, s_w.sha, sizeof(h)) != 0) {
        ESP_LOGW(TAG, "package sha256 mismatch");
        return "pkg_sha";
    }
    s_w.done = true;
    return NULL;
}

// 머리 128 B 를 flags=0xFFFFFFFF 로 쓰고, 그다음 flags 워드만 bit0 → 0. 중간에 전원이 끊기면 bit0=1 → 무시되는 슬롯
const char *dsp_pkg_finalize(uint32_t dspfw_crc, const char *ver)
{
    if (!s_w.active || !s_w.done) {
        return "pkg_flash";
    }
    pkg_hdr_t h;
    memset(&h, 0xFF, sizeof(h));
    memcpy(h.magic, HDR_MAGIC, sizeof(h.magic));
    h.flags     = 0xFFFFFFFF;
    h.seq       = s_w.seq;
    h.pkg_size  = s_w.size;
    h.job       = s_w.job;
    h.dspfw_crc = dspfw_crc;
    memset(h.pkg_id, 0, sizeof(h.pkg_id));
    memcpy(h.pkg_id, s_w.id, 16);
    memset(h.ver, 0, sizeof(h.ver));
    strlcpy(h.ver, ver, sizeof(h.ver));
    memcpy(h.sha256, s_w.sha, sizeof(h.sha256));
    const uint32_t base = slot_base(s_w.slot);
    const uint32_t fl = 0xFFFFFFFF & ~F_WRITTEN;
    const int slot = s_w.slot;
    const char *err = NULL;
    if (esp_partition_write(s_part, base, &h, sizeof(h)) != ESP_OK ||
        esp_partition_write(s_part, base + offsetof(pkg_hdr_t, flags), &fl, sizeof(fl)) != ESP_OK) {
        err = "pkg_flash";
    }
    memset(&s_w, 0, sizeof(s_w));
    reload_all();
    if (!err && (s_slot[slot].st != DSP_PKG_STAGED || s_slot[slot].dspfw_crc != dspfw_crc)) {
        err = "pkg_flash";                  // 다시 읽은 머리가 다름
    }
    if (!err) {
        ESP_LOGI(TAG, "slot %d: STAGED id %s ver %s (seq %lu)", slot, s_slot[slot].id, s_slot[slot].ver,
                 (unsigned long)s_slot[slot].seq);
    }
    return err;
}

// ───────── 적용 확인·표시 ─────────

const char *dsp_pkg_verify(int slot)
{
    if (!s_part) {
        return "pkg_nopart";
    }
    if (slot < 0 || slot >= DSP_PKG_SLOTS || s_slot[slot].st == DSP_PKG_EMPTY) {
        return "no_pkg";
    }
    const dsp_pkg_slot_t s = s_slot[slot];
    const uint32_t base = slot_base(slot) + DSP_PKG_HDR_SECTOR;
    static psa_hash_operation_t op;
    op = psa_hash_operation_init();
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return "pkg_sha";
    }
    uint32_t since = 0;
    for (uint32_t off = 0; off < s.size;) {
        size_t n = s.size - off < IO_BUF ? s.size - off : IO_BUF;
        if (esp_partition_read(s_part, base + off, s_buf, n) != ESP_OK) {
            psa_hash_abort(&op);
            return "pkg_flash";
        }
        if (psa_hash_update(&op, s_buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return "pkg_sha";
        }
        off += n;
        since += n;
        if (since >= YIELD_BYTES) {         // 6MB 해시는 1초 넘게 걸림 — 아이들 태스크(워치독)·중계 태스크에 틈
            since = 0;
            vTaskDelay(1);
        }
    }
    uint8_t h[32];
    size_t hl = 0;
    if (psa_hash_finish(&op, h, sizeof(h), &hl) != PSA_SUCCESS) {
        psa_hash_abort(&op);
        return "pkg_sha";
    }
    if (hl != sizeof(h) || memcmp(h, s.sha, sizeof(h)) != 0) {
        ESP_LOGW(TAG, "slot %d: package sha256 mismatch on flash", slot);
        return "pkg_sha";
    }
    return NULL;
}

static const char *clear_flag(int i, uint32_t bit)
{
    uint32_t fl;
    const uint32_t off = slot_base(i) + offsetof(pkg_hdr_t, flags);
    if (esp_partition_read(s_part, off, &fl, sizeof(fl)) != ESP_OK) {
        return "pkg_flash";
    }
    fl &= ~bit;
    return esp_partition_write(s_part, off, &fl, sizeof(fl)) == ESP_OK ? NULL : "pkg_flash";
}

const char *dsp_pkg_mark_applied(int slot)
{
    if (!s_part || slot < 0 || slot >= DSP_PKG_SLOTS || s_slot[slot].st != DSP_PKG_STAGED) {
        return "pkg_state";
    }
    const int other = 1 - slot;
    const bool other_applied = s_slot[other].st != DSP_PKG_EMPTY &&
                               !(s_slot[other].flags & F_APPLIED) && (s_slot[other].flags & F_OLD);
    const char *err = clear_flag(slot, F_APPLIED);
    if (!err && other_applied) {
        err = clear_flag(other, F_OLD);
    }
    reload_all();
    return err;
}

static size_t clamp(int n, size_t cap)
{
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

size_t dsp_pkg_format(char *buf, size_t cap, uint32_t staged_crc)
{
    if (cap == 0) {
        return 0;
    }
    if (!s_part) {
        return clamp(snprintf(buf, cap, "pkg : nopart - no '%s' partition, reflash over USB with the 0.9.0 partition table (16MB board)\n",
                              DSP_PKG_LABEL), cap);
    }
    const int pair = staged_crc ? dsp_pkg_find_staged(staged_crc) : -1;
    size_t n = 0;
    for (int i = 0; i < DSP_PKG_SLOTS; i++) {
        const dsp_pkg_slot_t s = s_slot[i];
        if (s.st == DSP_PKG_EMPTY) {
            n += clamp(snprintf(buf + n, cap - n, "pkg slot%d: empty\n", i), cap - n);
        } else {
            n += clamp(snprintf(buf + n, cap - n, "pkg slot%d: %s id=%s ver=%s size=%lu seq=%lu job=%lu%s\n", i,
                                dsp_pkg_state_name(s.st), s.id, s.ver, (unsigned long)s.size, (unsigned long)s.seq,
                                (unsigned long)s.job, i == pair ? " <- staged" : ""), cap - n);
        }
    }
    return n;
}

// ───────── 앱으로 내려받기 (읽기 전용) ─────────

// buf+*n 에 덧붙인다. 칸이 모자라면 false
static bool put(char *buf, size_t cap, size_t *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= cap - *n) {
        return false;
    }
    *n += (size_t)r;
    return true;
}

size_t dsp_pkg_format_json(char *buf, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    bool ok = true;
    if (!s_part) {
        ok = put(buf, cap, &n, "{\"err\":\"nopart\"}");
    } else {
        ok = put(buf, cap, &n, "{\"slots\":[");
        for (int i = 0; ok && i < DSP_PKG_SLOTS; i++) {
            const dsp_pkg_slot_t s = s_slot[i];
            ok = put(buf, cap, &n, i ? "," : "");
            if (ok && s.st == DSP_PKG_EMPTY) {
                ok = put(buf, cap, &n, "{\"i\":%d,\"st\":\"empty\"}", i);
            } else if (ok) {
                ok = put(buf, cap, &n, "{\"i\":%d,\"st\":\"%s\",\"id\":\"%s\",\"ver\":\"%s\",\"size\":%lu,\"seq\":%lu,\"job\":%lu,\"sha256\":\"",
                         i, dsp_pkg_state_name(s.st), s.id, s.ver, (unsigned long)s.size, (unsigned long)s.seq, (unsigned long)s.job);
                for (int k = 0; ok && k < 32; k++) {
                    ok = put(buf, cap, &n, "%02x", s.sha[k]);
                }
                ok = ok && put(buf, cap, &n, "\"}");
            }
        }
        ok = ok && put(buf, cap, &n, "]}");
    }
    if (!ok) {
        buf[0] = '\0';
        return 0;
    }
    return n;
}

static size_t err_line(char *out, size_t cap, const char *e)
{
    int r = snprintf(out, cap, "E %s\n", e);
    return (r > 0 && (size_t)r < cap) ? (size_t)r : 0;
}

size_t dsp_pkg_read_line(int slot, uint32_t off, uint32_t len, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    if (!s_part) {
        return err_line(out, cap, "nopart");
    }
    if (slot < 0 || slot >= DSP_PKG_SLOTS || s_slot[slot].st == DSP_PKG_EMPTY) {
        return err_line(out, cap, "slot");
    }
    const dsp_pkg_slot_t s = s_slot[slot];
    if (len < 1 || len > DSP_PKG_READ_MAX || off >= s.size) {
        return err_line(out, cap, "range");
    }
    const uint32_t n = s.size - off < len ? s.size - off : len;     // off < size 이므로 빼기는 안전
    uint8_t raw[DSP_PKG_READ_MAX];
    if (esp_partition_read(s_part, slot_base(slot) + DSP_PKG_HDR_SECTOR + off, raw, n) != ESP_OK) {
        return err_line(out, cap, "flash");
    }
    // dspimg_crc32_bytes 는 uint16_t 배열을 받는다 — 작은 배열로 나눠 먹여 스택을 아낀다
    uint16_t w[32];
    uint32_t c = dspimg_crc32_init();
    for (uint32_t i = 0; i < n;) {
        uint32_t k = n - i < 32 ? n - i : 32;
        for (uint32_t j = 0; j < k; j++) {
            w[j] = raw[i + j];
        }
        c = dspimg_crc32_bytes(c, w, k);
        i += k;
    }
    const uint32_t crc = dspimg_crc32_final(c);
    int h = snprintf(out, cap, "P %s %lu %lu %08lx ", s.id, (unsigned long)off, (unsigned long)n, (unsigned long)crc);
    if (h < 0 || (size_t)h >= cap) {
        return 0;
    }
    size_t b = bu_base64(raw, n, out + h, cap - (size_t)h);
    if (b == 0 || (size_t)h + b + 2 > cap) {
        return 0;
    }
    out[(size_t)h + b] = '\n';
    out[(size_t)h + b + 1] = '\0';
    return (size_t)h + b + 1;
}
