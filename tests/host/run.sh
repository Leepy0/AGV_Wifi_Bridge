#!/usr/bin/env bash
# 호스트 시험 (gcc·python3): 부트로더/브리지 공용 프로토콜, 앱 업데이트 진입 판정, 부트로더 진입 시뮬레이션,
# 부트로더 하드웨어 계층 문법 검사, 패커 selftest·기준 .dspfw 재현. CI(build.yml host-test)와 로컬에서 같이 쓴다.
#   tests/host/run.sh            (SAN=0 이면 주소·UB 검사기 끔)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HT="$ROOT/tests/host"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-gcc}
CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Werror -Wno-unknown-pragmas -I$ROOT/dsp/common -I$HT"
if [ "${SAN:-1}" = "1" ]; then
    CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all"
fi

echo "== bootproto / dsp_image"
$CC $CFLAGS "$ROOT/dsp/common/bootproto.c" "$ROOT/dsp/common/dsp_image.c" "$HT/test_bootproto.c" -o "$OUT/test_bootproto"
"$OUT/test_bootproto" "$HT/golden/app_golden.dspfw"

echo "== app_fwupdate (+ bridge main/dsp_app_proto.c frame contract)"
$CC $CFLAGS -I"$ROOT/dsp/app" -include "$HT/fwup_host.h" -c "$ROOT/dsp/app/app_fwupdate.c" -o "$OUT/app_fwupdate.o"
$CC $CFLAGS -I"$ROOT/dsp/app" -I"$ROOT/main" "$HT/test_fwup.c" "$OUT/app_fwupdate.o" "$ROOT/dsp/common/dsp_image.c" \
    "$ROOT/main/dsp_app_proto.c" -o "$OUT/test_fwup"
"$OUT/test_fwup"

echo "== boot_main (simulated hardware)"
$CC $CFLAGS -Wno-main -I"$ROOT/dsp/boot" -include "$HT/boot_host.h" -c "$ROOT/dsp/boot/boot_main.c" -o "$OUT/boot_main.o"
$CC $CFLAGS -I"$ROOT/dsp/boot" "$HT/sim_boot.c" "$OUT/boot_main.o" "$ROOT/dsp/common/bootproto.c" "$ROOT/dsp/common/dsp_image.c" -o "$OUT/sim_boot"
"$OUT/sim_boot"

echo "== boot_hw.c syntax (TI Flash API stub)"
$CC -std=c99 -fsyntax-only -Wall -Wextra -Werror -Wno-unknown-pragmas -Wno-int-to-pointer-cast \
    -I"$ROOT/dsp/common" -I"$ROOT/dsp/boot" -I"$HT/stubs" -include "$HT/c28x_host.h" "$ROOT/dsp/boot/boot_hw.c"
echo "ok"

echo "== dsp_pack.py selftest"
python3 "$ROOT/tools/dsp_pack.py" selftest

echo "== dsp_pack.py golden (같은 HEX·버전·빌드 시각 → 같은 .dspfw)"
python3 "$ROOT/tools/dsp_pack.py" pack "$HT/golden/app_golden.hex" -o "$OUT/golden.dspfw" --ver 1.2.3 --build 1700000000 2>/dev/null
cmp "$OUT/golden.dspfw" "$HT/golden/app_golden.dspfw"
python3 "$ROOT/tools/dsp_pack.py" info "$HT/golden/app_golden.dspfw"
python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$ROOT/tools/dsp_flash.py"

echo "host tests: all passed"
