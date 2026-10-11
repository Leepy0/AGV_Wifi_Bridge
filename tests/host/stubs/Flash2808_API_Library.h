#pragma once
// TI SPRC193 Flash API 헤더 자리표시자 — boot_hw.c 를 호스트 gcc 로 문법 검사할 때만 쓴다 (실제 빌드는 TI 헤더)
#include <stdint.h>
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef struct { Uint32 FirstFailAddr; Uint16 ExpectedData, ActualData; } FLASH_ST;
#define SECTORA 0x0001
#define SECTORB 0x0002
#define SECTORC 0x0004
#define SECTORD 0x0008
#define STATUS_SUCCESS 0
#define SCALE_FACTOR 0x0000
extern Uint32 Flash_CPUScaleFactor;
extern void (*Flash_CallbackPtr)(void);
Uint16 Flash_Erase(Uint16 SectorMask, FLASH_ST *FEraseStat);
Uint16 Flash_Program(Uint16 *FlashAddr, Uint16 *BuffAddr, Uint32 Length, FLASH_ST *FProgStatus);
Uint16 Flash_Verify(Uint16 *StartAddr, Uint16 *BuffAddr, Uint32 Length, FLASH_ST *FVerifyStat);
