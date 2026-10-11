#!/usr/bin/env python3
"""tools/dsp_pack.py 로 시험용 .dspfw 생성 (정상 + 결함) — test_dsp_app_proto 가 대조"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
import dsp_pack as dp  # noqa: E402

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
mem = {dp.APP_CODE_ADDR + i: (i * 31 + 5) & 0xFFFF for i in range(1000)}
for i in range(300, 600):           # 빈 구간 → 블록 생략
    mem[dp.APP_CODE_ADDR + i] = 0xFFFF
mem[dp.APP_CODE_ADDR + 3000] = 0x1234   # 멀리 떨어진 마지막 워드
good = dp.pack(mem, "1.2.3", 1700000000)
dp.unpack(good)                      # 패커 자체 검사 통과 확인
open(os.path.join(out, "good.dspfw"), "wb").write(good)


def refile(body: bytes) -> bytes:   # 파일 CRC·길이만 다시 맞춤 (블록 결함은 그대로)
    total = dp.FILE_HDR_LEN + len(body)
    return dp.FILE_MAGIC + struct.pack("<II", total, dp.crc32_final(dp.crc32_bytes(body))) + good[16:32] + body


body = bytearray(good[dp.FILE_HDR_LEN:])
first = dp.HDR_WORDS * 2
# 첫 블록 주소를 앱 영역 밖으로
b = bytearray(body); struct.pack_into("<I", b, first, 0x3F4000)
open(os.path.join(out, "bad_addr.dspfw"), "wb").write(refile(bytes(b)))
# 첫 블록 데이터 변조 (파일 CRC 는 맞음)
b = bytearray(body); b[first + 6] ^= 0xFF
open(os.path.join(out, "bad_crc.dspfw"), "wb").write(refile(bytes(b)))
# 꼬리에 쓰레기
open(os.path.join(out, "bad_tail.dspfw"), "wb").write(refile(bytes(body) + b"\0\0\0\0"))
print("images in", out)
