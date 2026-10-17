#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// 서버 시각 동기 (0.8.0+). 브리지에는 배터리 시계도 인터넷(NTP)도 없어 부팅하면 1970년에서 시작하므로, 서버(게이트웨이)의 시계를 기준으로 맞춘다.
//  - Wi-Fi 에 붙으면 TIME_REQ {"t0": 가동 ms} 를 보내고, 서버의 TIME {"t0","t1","t2"} 로 시각 = t2 + rtt/2 (NTP 한 번 교환).
//    맞출 때까지 2초 간격(6번), 그 뒤 30초 간격으로 다시 묻고(구 서버는 답이 없다), 맞춘 뒤에는 10분마다 다시 맞춘다.
//  - 서버가 편차를 보고 먼저 보내는 TIME {"t2"} 도 받는다.
// 맞춘 시각은 RAM 의 시스템 시계(settimeofday)에 둔다 — 재부팅하면 다시 맞춘다.
void   timesync_on_wifi_up(void);                           // STA 가 IP 를 받았을 때 (곧 다시 묻도록)
void   timesync_tick(void);                                 // heartbeat 태스크가 주기적으로 — 요청할 때가 되면 TIME_REQ 송신
bool   timesync_on_packet(const uint8_t *pl, size_t len);   // 서버 TIME 패킷. 시각을 맞췄으면 true
bool   timesync_now_ms(int64_t *unix_ms);                   // 맞춘 상태면 UTC unix ms
int    timesync_age_s(void);                                // 마지막으로 맞춘 뒤 초. 미동기 -1
size_t timesync_hb_fields(char *buf, size_t cap);           // heartbeat 에 붙일 조각: "tsync":N,"ts":X,  (ts 는 맞춘 뒤에만)
size_t timesync_format(char *buf, size_t cap);              // 텍스트 한 줄 "time: …\n" (콘솔·BLE·UDP 'time', 'stat')
size_t timesync_json(char *buf, size_t cap);                // stat json 의 "time" 객체 {..}. 넘치면 0
