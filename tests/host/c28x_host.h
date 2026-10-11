#pragma once
// C28x 전용 코드를 호스트 gcc 로 컴파일할 때 -include 로 넣는다: 인라인 어셈블리(EALLOW·SETC INTM 등)를 지운다
#include <stdint.h>
#define asm(x) ((void)0)
