#pragma once
// 브리지 공통 소형 유틸 (헤더 전용 — ESP-IDF 없이 호스트 gcc 로 시험: tests/bridge_util)
//  - OTA URL 호스트 확인 (결정 11: 호스트가 설정된 서버 IP 일 때만 받음)
//  - heartbeat 주기 hb_ms 허용 범위 (결정 14: 0 = 끔, 100~2000ms)
//  - 설정 로그인 연속 실패 계수 (결정 10: 같은 연결에서 5회 연속 실패 시 끊음)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define BU_HB_MS_MIN        100
#define BU_HB_MS_MAX        2000     // 서버 대기 시간(오프라인 5초·확정 15초)이 1초 주기를 전제
#define BU_LOGIN_MAX_FAILS  5

// heartbeat 주기 허용값: 0(끔, 디버그용) 또는 100~2000ms
static inline bool bu_hb_ms_valid(uint32_t v)
{
    return v == 0 || (v >= BU_HB_MS_MIN && v <= BU_HB_MS_MAX);
}

// 10진 dotted-quad (s[0..n)) → 네트워크 바이트 순서. 앞자리 0(예: 010)은 8진 해석 차이가 있어 거부
static inline bool bu_parse_ipv4(const char *s, size_t n, uint32_t *out_be)
{
    uint8_t o[4];
    size_t i = 0;
    for (int k = 0; k < 4; k++) {
        if (k > 0) {
            if (i >= n || s[i] != '.') return false;
            i++;
        }
        size_t start = i;
        unsigned v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && i - start < 3) {
            v = v * 10 + (unsigned)(s[i] - '0');
            i++;
        }
        size_t len = i - start;
        if (len == 0 || v > 255 || (len > 1 && s[start] == '0')) return false;
        o[k] = (uint8_t)v;
    }
    if (i != n) return false;
    memcpy(out_be, o, 4);       // 메모리 순서 = 네트워크 바이트 순서 (in_addr.s_addr 과 같음)
    return true;
}

// s 가 소문자 접두어 pre 로 시작하는지 (대소문자 무시 — URL 스킴은 대소문자를 가리지 않음)
static inline bool bu_prefix_ci(const char *s, const char *pre)
{
    for (; *pre; s++, pre++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *pre) return false;
    }
    return true;
}

// URL 이 http(s)://<서버 IP>[:포트][/...] 꼴이고 호스트가 server_ip_be(네트워크 바이트 순서)와 같은지.
// 호스트 부분은 숫자·점·콜론만 허용 — 사용자 정보(a@b), 호스트 이름, IPv6, 역슬래시 등 파서마다 해석이 다른 꼴은 모두 거부
static inline bool bu_url_host_is(const char *url, uint32_t server_ip_be)
{
    const char *p;
    if (url == NULL || server_ip_be == 0) return false;
    if (bu_prefix_ci(url, "http://")) {
        p = url + 7;
    } else if (bu_prefix_ci(url, "https://")) {
        p = url + 8;
    } else {
        return false;
    }
    size_t a = 0;               // authority 길이
    while (p[a] != '\0' && p[a] != '/' && p[a] != '?' && p[a] != '#') {
        char c = p[a];
        if (!((c >= '0' && c <= '9') || c == '.' || c == ':')) return false;
        a++;
    }
    const char *colon = memchr(p, ':', a);
    size_t hl = colon ? (size_t)(colon - p) : a;
    if (colon) {
        size_t pl = a - hl - 1;
        unsigned port = 0;
        if (pl == 0 || pl > 5) return false;
        for (size_t i = 0; i < pl; i++) {
            char c = colon[1 + i];
            if (c < '0' || c > '9') return false;
            port = port * 10 + (unsigned)(c - '0');
        }
        if (port == 0 || port > 65535) return false;
    }
    uint32_t ip;
    return bu_parse_ipv4(p, hl, &ip) && ip == server_ip_be;
}

// 같은 연결(session)에서의 로그인 연속 실패 계수
typedef struct {
    uint32_t session;
    uint8_t  fails;
} bu_login_guard_t;

// 실패 1회 기록. true = 연속 BU_LOGIN_MAX_FAILS 회 — 이 연결을 끊어야 함 (계수는 0 으로)
static inline bool bu_login_fail(bu_login_guard_t *g, uint32_t session)
{
    if (g->session != session) {
        g->session = session;
        g->fails = 0;
    }
    if (++g->fails >= BU_LOGIN_MAX_FAILS) {
        g->fails = 0;
        return true;
    }
    return false;
}

// 로그인 성공 → 계수 초기화
static inline void bu_login_ok(bu_login_guard_t *g, uint32_t session)
{
    g->session = session;
    g->fails = 0;
}
