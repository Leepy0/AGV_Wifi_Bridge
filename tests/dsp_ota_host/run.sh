#!/bin/sh
# 브리지 메인보드 OTA 검사 도우미 호스트 시험: gcc + python3
set -e
D=$(cd "$(dirname "$0")" && pwd)
R="$D/../.."
OUT="${TMPDIR:-/tmp}/dsp_ota_host.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -O1 -I"$R/main" -I"$R/dsp/common" \
    "$D/test_dsp_app_proto.c" "$R/main/dsp_app_proto.c" "$R/dsp/common/dsp_image.c" -o "$OUT/t"
python3 "$D/mkimg.py" "$OUT/img" 2>/dev/null
"$OUT/t" "$OUT/img/good.dspfw" "$OUT/img/bad_addr.dspfw" "$OUT/img/bad_crc.dspfw" "$OUT/img/bad_tail.dspfw"
# dsp_ota.c 흐름 (가짜 UART·HTTP·파티션 + 실제 bootproto.c 장치 처리기)
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror -O1 -I"$D/stub" -I"$R/main" -I"$R/dsp/common" \
    "$D/test_dsp_ota_flow.c" "$R/main/dsp_app_proto.c" "$R/dsp/common/dsp_image.c" "$R/dsp/common/bootproto.c" -o "$OUT/flow"
"$OUT/flow"
