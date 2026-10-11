// bridge_util.h 시험: OTA URL 호스트 확인(결정 11), hb_ms 범위(결정 14), 로그인 연속 실패(결정 10)
#include <string.h>
#include "bridge_util.h"
#include "test_common.h"

static uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint8_t o[4] = { a, b, c, d };
    uint32_t v;
    memcpy(&v, o, 4);       // 네트워크 바이트 순서 (in_addr.s_addr 과 같음)
    return v;
}

static void test_hb_ms(void)
{
    CHECK(bu_hb_ms_valid(0));
    CHECK(bu_hb_ms_valid(100));
    CHECK(bu_hb_ms_valid(1000));
    CHECK(bu_hb_ms_valid(2000));
    CHECK(!bu_hb_ms_valid(1));
    CHECK(!bu_hb_ms_valid(99));
    CHECK(!bu_hb_ms_valid(2001));
    CHECK(!bu_hb_ms_valid(10000));
    CHECK(!bu_hb_ms_valid(60000));
}

static void test_parse_ipv4(void)
{
    uint32_t v;
    CHECK(bu_parse_ipv4("192.168.0.10", 12, &v) && v == ip(192, 168, 0, 10));
    CHECK(bu_parse_ipv4("0.0.0.0", 7, &v) && v == 0);
    CHECK(bu_parse_ipv4("255.255.255.255", 15, &v) && v == ip(255, 255, 255, 255));
    CHECK(!bu_parse_ipv4("256.1.1.1", 9, &v));
    CHECK(!bu_parse_ipv4("1.2.3", 5, &v));
    CHECK(!bu_parse_ipv4("1.2.3.4.5", 9, &v));
    CHECK(!bu_parse_ipv4("1.2.3.", 6, &v));
    CHECK(!bu_parse_ipv4("01.2.3.4", 8, &v));      // 8진 해석 차이
    CHECK(!bu_parse_ipv4("1..3.4", 6, &v));
    CHECK(!bu_parse_ipv4("1234.1.1.1", 10, &v));
    CHECK(!bu_parse_ipv4("", 0, &v));
    CHECK(bu_parse_ipv4("10.0.0.1xyz", 8, &v) && v == ip(10, 0, 0, 1));   // 길이 n 까지만 봄
}

static void test_url_host(void)
{
    const uint32_t srv = ip(192, 168, 0, 10);
    // 허용
    CHECK(bu_url_host_is("http://192.168.0.10/api/firmware/a.bin", srv));
    CHECK(bu_url_host_is("http://192.168.0.10:8080/api/firmware/a.bin", srv));
    CHECK(bu_url_host_is("https://192.168.0.10/x.bin", srv));
    CHECK(bu_url_host_is("http://192.168.0.10", srv));
    CHECK(bu_url_host_is("http://192.168.0.10?f=1", srv));
    CHECK(bu_url_host_is("http://192.168.0.10:65535/", srv));
    // 다른 호스트·이름
    CHECK(!bu_url_host_is("http://192.168.0.11/a.bin", srv));
    CHECK(!bu_url_host_is("http://10.0.0.5/a.bin", srv));
    CHECK(!bu_url_host_is("http://gateway.local/a.bin", srv));
    CHECK(!bu_url_host_is("http://github.com/a.bin", srv));
    // 파서마다 해석이 다른 꼴
    CHECK(!bu_url_host_is("http://192.168.0.10@10.0.0.5/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10:80@10.0.0.5/a.bin", srv));
    CHECK(!bu_url_host_is("http://user:pw@192.168.0.10/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10\\@10.0.0.5/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.000.010/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10./a.bin", srv));
    CHECK(!bu_url_host_is("http://[::1]/a.bin", srv));
    CHECK(!bu_url_host_is("http:///a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10:/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10:0/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10:65536/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10:123456/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10:80:90/a.bin", srv));
    CHECK(!bu_url_host_is("http://192.168.0.10%2f/a.bin", srv));
    // 스킴
    CHECK(bu_url_host_is("HTTP://192.168.0.10/a.bin", srv));
    CHECK(!bu_url_host_is("ftp://192.168.0.10/a.bin", srv));
    CHECK(!bu_url_host_is("file:///a.bin", srv));
    CHECK(!bu_url_host_is("httpx://192.168.0.10/a.bin", srv));
    CHECK(!bu_url_host_is("192.168.0.10/a.bin", srv));
    CHECK(!bu_url_host_is("", srv));
    CHECK(!bu_url_host_is(NULL, srv));
    // 서버 IP 미설정(0)이면 아무것도 받지 않음
    CHECK(!bu_url_host_is("http://0.0.0.0/a.bin", 0));
}

static void test_login_guard(void)
{
    bu_login_guard_t g = {0};
    // 같은 연결에서 4회까지는 유지, 5회째 끊음
    for (int i = 0; i < BU_LOGIN_MAX_FAILS - 1; i++) {
        CHECK(!bu_login_fail(&g, 1));
    }
    CHECK(bu_login_fail(&g, 1));
    // 끊은 뒤 계수 초기화
    CHECK(!bu_login_fail(&g, 1));
    // 성공하면 초기화
    bu_login_ok(&g, 1);
    for (int i = 0; i < BU_LOGIN_MAX_FAILS - 1; i++) {
        CHECK(!bu_login_fail(&g, 1));
    }
    CHECK(bu_login_fail(&g, 1));
    // 새 연결이면 새로 셈 (첫 연결 session=1, 초기 guard session=0)
    bu_login_guard_t h = {0};
    for (int i = 0; i < BU_LOGIN_MAX_FAILS - 1; i++) {
        CHECK(!bu_login_fail(&h, 1));
    }
    CHECK(!bu_login_fail(&h, 2));     // 다른 연결 → 1회째
    for (int i = 0; i < BU_LOGIN_MAX_FAILS - 2; i++) {
        CHECK(!bu_login_fail(&h, 2));
    }
    CHECK(bu_login_fail(&h, 2));
}

int main(void)
{
    test_hb_ms();
    test_parse_ipv4();
    test_url_host();
    test_login_guard();
    DONE("test_bridge_util");
}
