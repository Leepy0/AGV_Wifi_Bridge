#pragma once
// app_fwupdate.c 를 호스트에서 컴파일할 때 -include 로 넣는다: C28x 전용 asm 을 지우고 고정 주소를 가짜 메모리로
#include "c28x_host.h"
volatile uint16_t *host_fwup_mem(uint32_t addr);
#define FWUP_MEM(a) host_fwup_mem((uint32_t)(a))
