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

static bool solve(const char *js, int64_t up_ms, int64_t pending, bu_time_t *t)
{
    return bu_time_solve(js, strlen(js), up_ms, pending, t);
}

static void test_json_i64(void)
{
    int64_t v = 0;
    CHECK(bu_json_i64("{\"t0\":123,\"t1\":-5}", 18, "t0", &v) && v == 123);
    CHECK(bu_json_i64("{\"t0\":123,\"t1\":-5}", 18, "t1", &v) && v == -5);
    CHECK(bu_json_i64("{\"t2\": 1760700000123}", 21, "t2", &v) && v == 1760700000123LL);   // 콜론 뒤 공백 허용
    CHECK(!bu_json_i64("{\"t0\":123}", 10, "t2", &v));                                       // 없는 키
    CHECK(!bu_json_i64("{\"t0\":\"x\"}", 10, "t0", &v));                                     // 정수가 아님
    CHECK(!bu_json_i64("{\"t0\":1234567890123456789012}", 28, "t0", &v));                    // 너무 긴 수
    CHECK(!bu_json_i64("{\"t0\":12", 8, "t0", &v) || v == 12);                               // 길이로 자른 입력 (뒤를 읽지 않음)
    CHECK(!bu_json_i64("{\"xt0\":1}", 9, "t0", &v));                                         // 키 일부와 겹쳐도 따옴표까지 맞아야 함
    CHECK(!bu_json_i64("{\"t0\":1}", 3, "t0", &v));                                          // len 이 짧으면 못 찾음
}

static void test_time_solve(void)
{
    bu_time_t t;
    // 요청 응답: 요청을 가동 1000ms 에 보냈고 4ms 걸려 1004 에 받음. 서버는 받아서 2ms 뒤에 보냄 → rtt = 4 − 2 = 2
    CHECK(solve("{\"t0\":1000,\"t1\":1760700000100,\"t2\":1760700000102}", 1004, 1000, &t));
    CHECK(t.unix_ms == 1760700000103LL && t.rtt_ms == 2 && !t.pushed);           // t2 + rtt/2
    // 요청과 짝이 안 맞는 응답(옛 요청)·요청한 적 없음 → 버림
    CHECK(!solve("{\"t0\":900,\"t1\":1760700000100,\"t2\":1760700000102}", 1004, 1000, &t));
    CHECK(!solve("{\"t0\":1000,\"t1\":1760700000100,\"t2\":1760700000102}", 1004, -1, &t));
    // t1 이 없으면 rtt 를 못 구함
    CHECK(!solve("{\"t0\":1000,\"t2\":1760700000102}", 1004, 1000, &t));
    // 같은 밀리초 안에 돌아와 −1 이 나와도 0 으로 (내림 오차), −6 이하는 거부
    CHECK(solve("{\"t0\":1000,\"t1\":1760700000100,\"t2\":1760700000101}", 1000, 1000, &t) && t.rtt_ms == 0 && t.unix_ms == 1760700000101LL);
    CHECK(!solve("{\"t0\":1000,\"t1\":1760700000100,\"t2\":1760700000107}", 1000, 1000, &t));
    // 너무 오래 걸린 응답(10초 초과)은 오래된 것으로 봄
    CHECK(solve("{\"t0\":1000,\"t1\":1760700000100,\"t2\":1760700000100}", 11000, 1000, &t) && t.rtt_ms == 10000);
    CHECK(!solve("{\"t0\":1000,\"t1\":1760700000100,\"t2\":1760700000100}", 11001, 1000, &t));
    // 서버가 먼저 보낸 {"t2"}: 그 값 그대로
    CHECK(solve("{\"t2\":1760700005000}", 123456, -1, &t) && t.unix_ms == 1760700005000LL && t.pushed && t.rtt_ms == -1);
    CHECK(solve("{\"t2\":1760700005000}", 123456, 777, &t) && t.pushed);          // 요청 중이어도 먼저 온 것은 받음
    // 서버 시계가 1970 근처·t2 없음·형식 틀림 → 버림
    CHECK(!solve("{\"t2\":1000}", 5, -1, &t));
    CHECK(!solve("{\"t0\":1,\"t1\":2}", 5, 1, &t));
    CHECK(!solve("garbage", 5, -1, &t));
}

int main(void)
{
    test_json_i64();
    test_time_solve();
    test_hb_ms();
    test_parse_ipv4();
    test_url_host();
    test_login_guard();
    DONE("test_bridge_util");
}
