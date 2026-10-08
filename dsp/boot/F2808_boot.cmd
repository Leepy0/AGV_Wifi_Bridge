/* 부트로더 링커 — 플래시 섹터 A 전용. 앱 영역(섹터 B~D)·CSM 영역은 할당하지 않는다.
   부트플래그 0x0007FC~0x0007FF 는 RAMM1 에서 제외 (앱 링커 cmd 도 동일) */
MEMORY
{
PAGE 0:
   RESET       : origin = 0x3FFFC0, length = 0x000002     /* 부트 ROM 리셋 벡터 (DSECT) */
   RAML0       : origin = 0x008000, length = 0x001000     /* ramfuncs + Flash API 실행 */
   FLASHA      : origin = 0x3F4000, length = 0x003F00     /* 부트로더 코드·상수 */
   BOOTINFO    : origin = 0x3F7F00, length = 0x000040     /* 정보 블록 (앱이 읽음) */
   CSM_RSVD    : origin = 0x3F7F80, length = 0x000076     /* 미할당 → 0xFFFF */
   BEGIN       : origin = 0x3F7FF6, length = 0x000002     /* 리셋 벡터 분기 */
   CSM_PWL     : origin = 0x3F7FF8, length = 0x000008     /* 미할당 → 0xFFFF (절대 0x0000 금지) */

PAGE 1:
   RAMM0       : origin = 0x000000, length = 0x000400
   RAMM1       : origin = 0x000400, length = 0x0003FC
   RAML1       : origin = 0x009000, length = 0x001000
   RAMH0       : origin = 0x3F8000, length = 0x002000
}

SECTIONS
{
   .reset      : > RESET,      PAGE = 0, TYPE = DSECT
   codestart   : > BEGIN,      PAGE = 0
   .bootinfo   : > BOOTINFO,   PAGE = 0
   .text       : > FLASHA,     PAGE = 0
   .cinit      : > FLASHA,     PAGE = 0
   .pinit      : > FLASHA,     PAGE = 0
   .econst     : > FLASHA,     PAGE = 0
   .switch     : > FLASHA,     PAGE = 0

   ramfuncs    : LOAD = FLASHA, RUN = RAML0, PAGE = 0,
                 LOAD_START(_RamfuncsLoadStart), LOAD_END(_RamfuncsLoadEnd), RUN_START(_RamfuncsRunStart)
   Flash28_API : { -l Flash2808_API_V302.lib (.econst)
                   -l Flash2808_API_V302.lib (.text) }
                 LOAD = FLASHA, RUN = RAML0, PAGE = 0,
                 LOAD_START(_Flash28_API_LoadStart), LOAD_END(_Flash28_API_LoadEnd), RUN_START(_Flash28_API_RunStart)

   .stack      : > RAMM0,      PAGE = 1
   .ebss       : > RAML1,      PAGE = 1
   .esysmem    : > RAMH0,      PAGE = 1
   .bss        : > RAMM1,      PAGE = 1
   .const      : > FLASHA,     PAGE = 0
   .sysmem     : > RAMH0,      PAGE = 1
}
