// 메인보드 앱 쪽 펌웨어 업데이트 지원 — C28x (char = 16비트)
#include "app_fwupdate.h"
#include "dsp_image.h"

#define STX 0x02
#define ETX 0x03

// 앱 헤더 자리표시자 (0x3E8000, 64 워드). 패커가 실제 값(버전·길이·CRC)으로 덮어쓴다
#pragma DATA_SECTION(g_apphdr, ".apphdr")
const uint16_t g_apphdr[DSPIMG_HDR_WORDS] = { DSPIMG_MAGIC0, DSPIMG_MAGIC1 };

static char s_app_ver[DSPIMG_VER_WORDS];
static char s_boot_ver[17];

char fwup_bcc(const char *payload, uint16_t len)
{
    uint16_t sum = 0, i, n;
    for (i = 0; i < len; i++) {
        sum += (uint16_t)payload[i] & 0xFF;
    }
    n = sum & 0x0F;
    return (char)(n < 10 ? '0' + n : 'A' + n - 10);
}

const char *fwup_app_version(void)
{
    const uint16_t *hdr = (const uint16_t *)DSPIMG_APP_HDR_ADDR;
    uint16_t i;
    if (hdr[DSPIMG_W_MAGIC0] != DSPIMG_MAGIC0 || hdr[DSPIMG_W_MAGIC1] != DSPIMG_MAGIC1) {
        return "-";
    }
    for (i = 0; i < DSPIMG_VER_WORDS - 1; i++) {
        s_app_ver[i] = (char)(hdr[DSPIMG_W_VER + i] & 0xFF);
        if (s_app_ver[i] == 0) {
            break;
        }
    }
    s_app_ver[DSPIMG_VER_WORDS - 1] = 0;
    return s_app_ver[0] ? s_app_ver : "-";
}

const char *fwup_boot_version(void)
{
    const uint16_t *info = (const uint16_t *)DSPIMG_BOOTINFO_ADDR;
    uint16_t i;
    if (info[0] != DSPIMG_BOOT_MAGIC0 || info[1] != DSPIMG_BOOT_MAGIC1) {
        return "-";
    }
    for (i = 0; i < 16; i++) {
        s_boot_ver[i] = (char)(info[DSPIMG_BOOT_W_VER + i] & 0xFF);
        if (s_boot_ver[i] == 0) {
            break;
        }
    }
    s_boot_ver[16] = 0;
    return s_boot_ver[0] ? s_boot_ver : "-";
}

// 8자 고정폭: 짧으면 공백, 길면 자름
static void put_fixed(char *dst, const char *src, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        dst[i] = *src ? *src++ : ' ';
    }
}

// unix 초 → YYYYMMDD (UTC)
static void put_date(char *dst, uint32_t t)
{
    uint32_t days = t / 86400UL;
    // 날짜 계산 (Howard Hinnant, civil_from_days)
    int32_t z = (int32_t)days + 719468L;
    int32_t era = (z >= 0 ? z : z - 146096L) / 146097L;
    uint32_t doe = (uint32_t)(z - era * 146097L);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400L;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    uint16_t i;
    if (m <= 2) {
        y++;
    }
    for (i = 0; i < 4; i++) { dst[3 - i] = (char)('0' + y % 10); y /= 10; }
    dst[4] = (char)('0' + m / 10); dst[5] = (char)('0' + m % 10);
    dst[6] = (char)('0' + d / 10); dst[7] = (char)('0' + d % 10);
}

int fwup_handle(const char *payload, uint16_t len, uint16_t agv_id, fwup_safe_fn safe,
                char *reply, uint16_t cap, uint16_t *reply_len)
{
    char body[32];
    uint16_t blen = 0, i, n;
    int result = FWUP_REPLY;

    // 헤더 5 + 본문 ≥ 1 + 공백 + BCC
    if (len < 8 || payload[0] != 'F' || cap < FWUP_REPLY_MAX) {
        return FWUP_NONE;
    }
    if (payload[len - 1] != fwup_bcc(payload, (uint16_t)(len - 1))) {
        return FWUP_NONE;               // 체크섬 오류: 무응답
    }
    if ((uint16_t)(payload[3] - '0') * 10 + (uint16_t)(payload[4] - '0') != agv_id) {
        return FWUP_NONE;               // 다른 호기
    }

    switch (payload[5]) {
    case 'V': {
        const uint16_t *hdr = (const uint16_t *)DSPIMG_APP_HDR_ADDR;
        uint32_t build = (uint32_t)hdr[DSPIMG_W_BUILD_LO] | ((uint32_t)hdr[DSPIMG_W_BUILD_HI] << 16);
        body[0] = 'V';
        put_fixed(&body[1], fwup_app_version(), 8);
        put_fixed(&body[9], fwup_boot_version(), 8);
        if (hdr[DSPIMG_W_MAGIC0] == DSPIMG_MAGIC0 && build != 0xFFFFFFFFUL) {
            put_date(&body[17], build);
        } else {
            put_fixed(&body[17], "-", 8);
        }
        blen = 25;
        break;
    }
    case 'U': {
        uint16_t reason = safe ? safe() : 0;
        body[0] = 'U';
        if (reason == 0) {
            body[1] = 'O'; body[2] = 'K'; body[3] = '0'; body[4] = '0';
            result = FWUP_REPLY_RESET;
        } else {
            body[1] = 'N'; body[2] = 'G';
            body[3] = (char)('0' + (reason / 10) % 10);
            body[4] = (char)('0' + reason % 10);
        }
        blen = 5;
        break;
    }
    default:
        return FWUP_NONE;
    }

    // R + PAN + MSG + AGV(2) + 본문 + 공백 + BCC, STX/ETX 로 감쌈
    n = 0;
    reply[n++] = STX;
    reply[n++] = 'R';
    reply[n++] = payload[1];
    reply[n++] = payload[2];
    reply[n++] = payload[3];
    reply[n++] = payload[4];
    for (i = 0; i < blen; i++) {
        reply[n++] = body[i];
    }
    reply[n++] = ' ';
    reply[n] = fwup_bcc(&reply[1], (uint16_t)(n - 1));
    n++;
    reply[n++] = ETX;
    *reply_len = n;
    return result;
}

void fwup_reset(void)
{
    volatile uint16_t *f = (volatile uint16_t *)DSPIMG_BOOTFLAG_ADDR;
    f[0] = DSPIMG_BOOTFLAG_0;
    f[1] = DSPIMG_BOOTFLAG_1;
    f[2] = DSPIMG_BOOTFLAG_2;
    f[3] = DSPIMG_BOOTFLAG_3;
    asm(" SETC INTM");
    asm(" EALLOW");
    *(volatile uint16_t *)0x7022 = 0x0000;      // SCSR: 워치독 → 리셋
    *(volatile uint16_t *)0x7029 = 0x0000;      // WDCR: WDCHK 오류 → 즉시 리셋
    asm(" EDIS");
    for (;;) {
    }
}
