; 진입점 — "codestart" 섹션(2 워드)은 플래시 BEGIN(부트로더 0x3F7FF6 / 앱 0x3E8040)에 배치.
; 워치독 정지 루틴은 .text 에 두고 분기한다 (controlSUITE DSP280x_CodeStartBranch.asm 과 같은 구조).
; 앱 프로젝트에 이미 DSP280x_CodeStartBranch.asm 이 있으면 그 파일을 그대로 쓰면 된다.
    .ref _c_int00
    .global code_start
    .sect "codestart"
code_start:
    LB wd_disable           ; 2 워드

    .text
wd_disable:
    SETC OBJMODE            ; 28x 오브젝트 모드
    EALLOW
    MOVZ DP, #7029h>>6      ; WDCR 데이터 페이지
    MOV @7029h, #0068h      ; WDDIS = 1, WDCHK = 101
    EDIS
    LB _c_int00
