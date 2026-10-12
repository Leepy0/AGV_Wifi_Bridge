#pragma once
// boot_main.c 를 호스트에서 컴파일할 때 -include 로 넣는다: C28x 전용 asm 을 지우고, 고정 주소를 가짜 메모리로,
// main 을 boot_main 으로 (시뮬레이터가 여러 번 부른다)
#include "c28x_host.h"
volatile uint16_t *host_boot_mem(uint32_t addr);
#define BOOT_MEM(a) host_boot_mem((uint32_t)(a))
#define main boot_main
