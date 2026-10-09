/* 앱 링커 예시 — 기존 앱 cmd 에서 FLASH 쪽만 이렇게 바꾼다.
   앱 = 섹터 D·C·B (0x3E8000~0x3F3FFF). 섹터 A(부트로더)·BEGIN·CSM 은 할당하지 않는다.
   부트플래그 0x0007FC~0x0007FF 는 RAMM1 에서 제외 (부트로더 cmd 와 동일) */
MEMORY
{
PAGE 0:
   RESET       : origin = 0x3FFFC0, length = 0x000002     /* 부트 ROM 리셋 벡터 (DSECT) */
   RAML0       : origin = 0x008000, length = 0x001000
   APPHDR      : origin = 0x3E8000, length = 0x000040     /* 앱 헤더 (패커가 채움) */
   APPBEGIN    : origin = 0x3E8040, length = 0x000002     /* codestart: 부트로더가 여기로 점프 */
   FLASHAPP    : origin = 0x3E8042, length = 0x00BFBE     /* 섹터 D 나머지 + C + B */
   /* 사용 금지: FLASHA 0x3F4000, CSM_RSVD 0x3F7F80, BEGIN 0x3F7FF6, CSM_PWL 0x3F7FF8 */

PAGE 1:
   RAMM0       : origin = 0x000000, length = 0x000400
   RAMM1       : origin = 0x000400, length = 0x0003FC     /* 0x7FC~0x7FF 부트플래그 */
   RAML1       : origin = 0x009000, length = 0x001000
   RAMH0       : origin = 0x3F8000, length = 0x002000
}

SECTIONS
{
   .reset      : > RESET,      PAGE = 0, TYPE = DSECT
   .apphdr     : > APPHDR,     PAGE = 0
   codestart   : > APPBEGIN,   PAGE = 0      /* DSP280x_CodeStartBranch.asm 의 "codestart" 섹션 */
   .text       : > FLASHAPP,   PAGE = 0
   .cinit      : > FLASHAPP,   PAGE = 0
   .pinit      : > FLASHAPP,   PAGE = 0
   .econst     : > FLASHAPP,   PAGE = 0
   .switch     : > FLASHAPP,   PAGE = 0
   ramfuncs    : LOAD = FLASHAPP, RUN = RAML0, PAGE = 0,
                 LOAD_START(_RamfuncsLoadStart), LOAD_END(_RamfuncsLoadEnd), RUN_START(_RamfuncsRunStart)

   .stack      : > RAMM0,      PAGE = 1
   .ebss       : > RAML1,      PAGE = 1
   .esysmem    : > RAMH0,      PAGE = 1
   .bss        : > RAMM1,      PAGE = 1
   .const      : > FLASHAPP,   PAGE = 0
   .sysmem     : > RAMH0,      PAGE = 1

   /* PIE 벡터 등 기존 DSP280x_Headers_nonBIOS.cmd 의 레지스터 섹션은 그대로 둔다 */
}
