#!/usr/bin/env python3
"""메인보드(TMS320F2808) 앱 이미지 패커

CCS 빌드 결과 → OTA 이미지(.dspfw). 형식은 docs/DSP_OTA_DESIGN.md 3절, dsp/common/dsp_image.h 와 같다.

  입력 만들기 (CCS 빌드 후 또는 post-build step):
    hex2000 -i -o app.hex app.out --memwidth=16 --romwidth=16
  패킹:
    dsp_pack.py pack app.hex -o app_1.2.3.dspfw --ver 1.2.3
  확인:
    dsp_pack.py info app_1.2.3.dspfw
    dsp_pack.py selftest        # CRC 테스트 벡터 (C 구현과 대조용)

hex2000 Intel HEX 는 --memwidth=16 --romwidth=16 일 때 주소가 워드 단위이고 각 워드가
상위 바이트 → 하위 바이트 순으로 기록된다 (실측). 바이트 주소(0x7D0000 대)로 나오면 자동 변환한다.
"""
import argparse
import struct
import sys
import time

APP_HDR_ADDR   = 0x3E8000
APP_CODE_ADDR  = 0x3E8040
APP_END        = 0x3F4000
HDR_WORDS      = 64
MAGIC0, MAGIC1 = 0xA55A, 0x4650
HDR_VER        = 1
VER_WORDS      = 32
BLOCK_WORDS    = 128
FILE_MAGIC     = b"AGVDSP1\0"
FILE_HDR_LEN   = 32


# ───────── CRC (dsp_image.c 와 동일 규칙: 워드마다 하위 바이트 → 상위 바이트) ─────────

def crc32_bytes(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc


def crc32_final(crc: int) -> int:
    return crc ^ 0xFFFFFFFF


def crc32_words(words) -> int:
    return crc32_final(crc32_bytes(words_to_le(words)))


def crc16_bytes(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc16_words(words) -> int:
    return crc16_bytes(words_to_le(words))


def words_to_le(words) -> bytes:
    return b"".join(struct.pack("<H", w) for w in words)


# ───────── Intel HEX ─────────

def parse_intel_hex(path: str) -> dict:
    """워드 주소 → 워드 값. hex2000 --memwidth=16 --romwidth=16 출력 기준 (워드당 2바이트, 상위 먼저)."""
    mem = {}
    base = 0
    with open(path, "r", encoding="ascii") as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if line[0] != ":":
                raise ValueError(f"{path}:{ln}: Intel HEX 형식이 아님")
            rec = bytes.fromhex(line[1:])
            if (sum(rec) & 0xFF) != 0:
                raise ValueError(f"{path}:{ln}: 체크섬 오류")
            n, addr, typ, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:-1]
            if len(data) != n:
                raise ValueError(f"{path}:{ln}: 길이 불일치")
            if typ == 0x00:
                if n % 2:
                    raise ValueError(f"{path}:{ln}: 데이터가 워드 단위가 아님 (--memwidth=16 --romwidth=16 필요)")
                a = base + addr
                for i in range(0, n, 2):
                    mem[a + i // 2] = (data[i] << 8) | data[i + 1]
            elif typ == 0x01:
                break
            elif typ == 0x02:
                base = ((data[0] << 8) | data[1]) << 4
            elif typ == 0x04:
                base = ((data[0] << 8) | data[1]) << 16
            elif typ in (0x03, 0x05):
                pass
            else:
                raise ValueError(f"{path}:{ln}: 지원하지 않는 레코드 타입 {typ:#x}")
    if not mem:
        raise ValueError("데이터가 없음")
    # 바이트 주소로 나온 경우(0x7D0000 대) → 워드 주소
    lo, hi = min(mem), max(mem)
    if lo >= APP_HDR_ADDR * 2 and hi < 0x800000:
        mem = {a // 2: v for a, v in mem.items()}
    return mem


# ───────── 이미지 ─────────

def build_header(app_len: int, app_crc: int, build_time: int, ver: str, flags: int = 0) -> list:
    if len(ver.encode("ascii")) > VER_WORDS - 1:
        raise ValueError("버전 문자열은 31자 이하")
    hdr = [0xFFFF] * HDR_WORDS
    hdr[0], hdr[1], hdr[2], hdr[3] = MAGIC0, MAGIC1, HDR_VER, flags
    hdr[4], hdr[5] = app_len & 0xFFFF, app_len >> 16
    hdr[6], hdr[7] = app_crc & 0xFFFF, app_crc >> 16
    hdr[8], hdr[9] = build_time & 0xFFFF, build_time >> 16
    vb = ver.encode("ascii")
    for i in range(VER_WORDS):
        hdr[10 + i] = vb[i] if i < len(vb) else 0
    c = crc16_words(hdr[:62])
    hdr[62], hdr[63] = c, (~c) & 0xFFFF
    return hdr


def parse_header(hdr: list) -> dict:
    if hdr[0] != MAGIC0 or hdr[1] != MAGIC1 or hdr[2] != HDR_VER:
        raise ValueError("앱 헤더 magic 불일치")
    c = crc16_words(hdr[:62])
    if hdr[62] != c or hdr[63] != ((~c) & 0xFFFF):
        raise ValueError("앱 헤더 CRC 불일치")
    ver = bytes(w & 0xFF for w in hdr[10:42]).split(b"\0", 1)[0].decode("ascii", "replace")
    return {
        "flags": hdr[3],
        "app_len": hdr[4] | (hdr[5] << 16),
        "app_crc": hdr[6] | (hdr[7] << 16),
        "build_time": hdr[8] | (hdr[9] << 16),
        "ver": ver,
    }


def pack(mem: dict, ver: str, build_time: int) -> bytes:
    bad = [a for a in mem if not (APP_HDR_ADDR <= a < APP_END)]
    if bad:
        raise ValueError(f"앱 영역(0x{APP_HDR_ADDR:X}~0x{APP_END - 1:X}) 밖의 데이터 {len(bad)}워드: 0x{min(bad):X}... "
                         "링커 cmd 가 앱용(섹터 B~D)인지 확인")
    in_hdr = [a for a in mem if a < APP_CODE_ADDR]
    if in_hdr:
        print(f"주의: 앱 헤더 영역에 {len(in_hdr)}워드가 있음 (.apphdr 자리표시자) — 패커 헤더로 덮어씀", file=sys.stderr)
    code = {a: v for a, v in mem.items() if a >= APP_CODE_ADDR}
    if not code:
        raise ValueError("앱 코드가 없음")
    if APP_CODE_ADDR not in code:
        raise ValueError(f"0x{APP_CODE_ADDR:X} 에 codestart 가 없음 — 링커 cmd 의 codestart 배치 확인")
    app_len = max(code) + 1 - APP_CODE_ADDR
    flat = [code.get(APP_CODE_ADDR + i, 0xFFFF) for i in range(app_len)]
    app_crc = crc32_words(flat)
    hdr = build_header(app_len, app_crc, build_time, ver)

    blocks = b""
    nblk = 0
    for off in range(0, app_len, BLOCK_WORDS):
        chunk = flat[off:off + BLOCK_WORDS]
        if all(w == 0xFFFF for w in chunk):
            continue            # 지워진 플래시와 같으므로 전송 생략
        blocks += struct.pack("<IH", APP_CODE_ADDR + off, len(chunk)) + words_to_le(chunk)
        nblk += 1

    body = words_to_le(hdr) + blocks
    total = FILE_HDR_LEN + len(body)
    vb = ver.encode("ascii")[:15].ljust(16, b"\0")
    fhdr = FILE_MAGIC + struct.pack("<II", total, crc32_final(crc32_bytes(body))) + vb
    print(f"app: {app_len} words ({app_len * 2} B), crc32 {app_crc:08X}, {nblk} blocks, file {total} B", file=sys.stderr)
    return fhdr + body


def unpack(data: bytes) -> dict:
    if len(data) < FILE_HDR_LEN + HDR_WORDS * 2 or data[:8] != FILE_MAGIC:
        raise ValueError(".dspfw 형식이 아님")
    total, fcrc = struct.unpack_from("<II", data, 8)
    if total != len(data):
        raise ValueError(f"길이 불일치: 헤더 {total}, 파일 {len(data)}")
    if crc32_final(crc32_bytes(data[FILE_HDR_LEN:])) != fcrc:
        raise ValueError("파일 CRC 불일치")
    hdr = list(struct.unpack_from("<64H", data, FILE_HDR_LEN))
    info = parse_header(hdr)
    pos = FILE_HDR_LEN + HDR_WORDS * 2
    blocks = []
    while pos < len(data):
        addr, n = struct.unpack_from("<IH", data, pos)
        pos += 6
        if n == 0 or n > BLOCK_WORDS or pos + n * 2 > len(data):
            raise ValueError(f"블록 손상 @{pos}")
        if not (APP_CODE_ADDR <= addr and addr + n <= APP_END):
            raise ValueError(f"블록 주소 범위 밖: 0x{addr:X}")
        blocks.append((addr, list(struct.unpack_from(f"<{n}H", data, pos))))
        pos += n * 2
    # 블록으로 복원한 코드의 CRC 가 헤더와 맞는지
    flat = [0xFFFF] * info["app_len"]
    for addr, ws in blocks:
        for i, w in enumerate(ws):
            if addr + i - APP_CODE_ADDR < info["app_len"]:
                flat[addr + i - APP_CODE_ADDR] = w
    if crc32_words(flat) != info["app_crc"]:
        raise ValueError("블록 데이터 CRC 가 앱 헤더와 불일치")
    info["blocks"] = blocks
    info["hdr_words"] = hdr
    return info


# ───────── 명령 ─────────

def cmd_pack(a):
    mem = parse_intel_hex(a.hexfile)
    build = int(a.build) if a.build else int(time.time())
    out = pack(mem, a.ver, build)
    with open(a.output, "wb") as f:
        f.write(out)
    print(f"wrote {a.output}", file=sys.stderr)


def cmd_info(a):
    with open(a.file, "rb") as f:
        info = unpack(f.read())
    words = sum(len(ws) for _, ws in info["blocks"])
    print(f"ver        : {info['ver']}")
    print(f"build_time : {info['build_time']} ({time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(info['build_time']))})")
    print(f"app_len    : {info['app_len']} words ({info['app_len'] * 2} B), end 0x{APP_CODE_ADDR + info['app_len'] - 1:X}")
    print(f"app_crc32  : {info['app_crc']:08X}")
    print(f"blocks     : {len(info['blocks'])} ({words} words transferred)")


def cmd_selftest(a):
    # 표준 검사값: CRC32("123456789") = CBF43926, CRC-16/CCITT-FALSE("123456789") = 29B1
    assert crc32_final(crc32_bytes(b"123456789")) == 0xCBF43926
    assert crc16_bytes(b"123456789") == 0x29B1
    words = [0x3231, 0x3433, 0x3635, 0x3837]          # "12345678" 을 하위 바이트 먼저 워드로
    print(f"crc32_words(12345678)  = {crc32_words(words):08X}")
    print(f"crc16_words(12345678)  = {crc16_words(words):04X}")
    hdr = build_header(0x1234, 0xDEADBEEF, 1700000000, "1.2.3")
    print(f"hdr[62] (hdr_crc16)    = {hdr[62]:04X}")
    print("selftest ok")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    q = sp.add_parser("pack", help="Intel HEX → .dspfw")
    q.add_argument("hexfile")
    q.add_argument("-o", "--output", required=True)
    q.add_argument("--ver", required=True, help="앱 버전 문자열 (31자 이하)")
    q.add_argument("--build", help="빌드 시각 unix 초 (기본: 지금)")
    q.set_defaults(fn=cmd_pack)
    q = sp.add_parser("info", help=".dspfw 내용 표시·검증")
    q.add_argument("file")
    q.set_defaults(fn=cmd_info)
    q = sp.add_parser("selftest", help="CRC 테스트 벡터")
    q.set_defaults(fn=cmd_selftest)
    a = p.parse_args()
    try:
        a.fn(a)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
