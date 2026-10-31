#!/usr/bin/env python3
"""메인보드(TMS320F2808) 앱 이미지 패커

CCS 빌드 결과 → OTA 이미지(.dspfw). 형식은 docs/DSP_OTA_DESIGN.md 3절, dsp/common/dsp_image.h 와 같다.

  배포용 패키지 (.dsppkg = manifest + .dspfw + .out + .map) — 메인보드 프로그램은 반드시 이 형식으로 낸다:
    dsp_pack.py bundle app.out --map app.map --ver 1.2.3 [--hex2000 <hex2000 경로>] [-o app_1.2.3.dsppkg]
      (.out 에서 hex2000 을 직접 돌려 .dspfw 를 만들어 .out/.map 과 한 파일로 묶는다. 규격: docs/DSP_OTA_DESIGN.md 3-1절)
    dsp_pack.py verify app_1.2.3.dsppkg    # 패키지 전체 검증 (종료 코드 0/1)
  확인:
    dsp_pack.py info app_1.2.3.dsppkg      # 패키지 또는 .dspfw 내용 표시·검증
    dsp_pack.py selftest        # CRC 테스트 벡터 (C 구현과 대조용), 패키지 시험

  벤치용 (.dspfw 만 만들기):
    hex2000 -i -o app.hex app.out --memwidth=16 --romwidth=16
    dsp_pack.py pack app.hex -o app_1.2.3.dspfw --ver 1.2.3

hex2000 Intel HEX 는 --memwidth=16 --romwidth=16 일 때 주소가 워드 단위이고 각 워드가
상위 바이트 → 하위 바이트 순으로 기록된다 (실측). 바이트 주소(0x7D0000 대)로 나오면 거부한다.
"""
import argparse
import calendar
import contextlib
import datetime
import glob
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import warnings
import zipfile
import zlib

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
# 버전 문자열 규칙: 앱 R 응답의 버전 칸이 8자 고정(app_fwupdate.c)이고 브리지·서버가 그 값을 이미지 버전과 비교하므로
# 8자를 넘으면 굽기는 성공해도 서버가 '버전 다름'으로 실패 처리한다. 빈 문자열·공백도 막는다.
VER_RE         = re.compile(r"[0-9A-Za-z._-]{1,8}")
VER_RULE       = "영문·숫자·. _ - 로 1~8자 (예: 1.2.3)"


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
    # 바이트 주소 HEX(--romwidth=8 등, 0x7D0000 대)는 받지 않는다: 워드 재조립과 바이트 순서를 추측하면
    # CRC 는 맞는데 내용이 틀린 이미지가 만들어져 기동 불능 앱이 올라갈 수 있다
    lo, hi = min(mem), max(mem)
    if lo >= APP_HDR_ADDR * 2 and hi < 0x800000:
        raise ValueError(f"바이트 주소 HEX (0x{lo:06X}~0x{hi:06X}) — hex2000 에 --memwidth=16 --romwidth=16 을 주어 워드 주소로 뽑으세요")
    return mem


# ───────── 이미지 ─────────

def check_ver(ver: str) -> str:
    if not isinstance(ver, str) or VER_RE.fullmatch(ver) is None:
        raise ValueError(f"버전 문자열 {ver!r} 은 쓸 수 없음 — {VER_RULE}")
    return ver


def build_header(app_len: int, app_crc: int, build_time: int, ver: str, flags: int = 0) -> list:
    check_ver(ver)
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
    check_ver(ver)
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
    nxt = APP_CODE_ADDR   # 블록은 주소 오름차순·겹침 없음 (브리지 dap_image_check_blocks, 게이트웨이 DspfwImage.Parse 와 같은 규칙)
    while pos < len(data):
        addr, n = struct.unpack_from("<IH", data, pos)
        pos += 6
        if n == 0 or n > BLOCK_WORDS or pos + n * 2 > len(data):
            raise ValueError(f"블록 손상 @{pos}")
        if not (APP_CODE_ADDR <= addr and addr + n <= APP_END):
            raise ValueError(f"블록 주소 범위 밖: 0x{addr:X}")
        if addr < nxt:
            raise ValueError(f"블록 순서·겹침 오류: 0x{addr:X} < 0x{nxt:X}")
        nxt = addr + n
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


# ───────── 패키지 (.dsppkg) — 규격: docs/DSP_OTA_DESIGN.md 3-1절 ─────────
# 메인보드 프로그램은 .dspfw 만 들어가지 않고 .out(DWARF)·.map 을 반드시 함께 보관한다 (원본 소스 유실 시 복구용).
# 만드는 쪽(bundle)과 읽는 쪽(verify_package — dsp_usb_stage.py·dsp_flash.py·게이트웨이)이 같은 규칙을 쓴다.

PKG_FORMAT        = "agv-dsppkg"
PKG_FORMAT_VER    = 1
PKG_MANIFEST      = "manifest.json"
PKG_NAME_RE       = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,59}")
STAGE_HDR_LEN     = 64                                  # 브리지 dsp_ota.c STAGE_HDR_LEN
STAGE_PART_SIZE   = 0x60000                             # partitions.csv 의 dspfw 크기
DSPFW_MAX         = STAGE_PART_SIZE - STAGE_HDR_LEN     # 393,152 B
# 압축 해제 기준 한도 (zip bomb 방지) — 실제로 읽으면서 센다
PKG_LIMITS        = {"manifest": 64 * 1024, "dspfw": DSPFW_MAX, "out": 64 << 20, "map": 16 << 20}
PKG_KIND_LABEL    = {"manifest": "manifest.json", "dspfw": ".dspfw", "out": ".out", "map": ".map"}
PKG_LINK_MODES    = ("time", "name", "skipped")
COFF_TARGET_C2000 = 0x009D                              # 알고 있는 값 — 다르면 경고만
ELF_MACHINE_C2000 = 141                                 # EM_TI_C2000
LINK_TOL_SEC      = 5                                   # 링크 시각 허용 오차
LINK_TZ_STEP      = 900                                 # 시간대 오프셋 격자 (15분)
LINK_TZ_MAX_K     = 56                                  # ±14시간
HEX2000_TIMEOUT   = 300

_MONTHS   = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
_WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
_CTIME_RE = re.compile(r"([A-Za-z]{3}) ([A-Za-z]{3}) (\d{1,2}) (\d{2}):(\d{2}):(\d{2}) (\d{4})", re.ASCII)


def _basename(p: str) -> str:
    return re.split(r"[/\\]", p)[-1]


def _utc_str(ts: int) -> str:
    return (datetime.datetime(1970, 1, 1) + datetime.timedelta(seconds=ts)).strftime("%Y-%m-%d %H:%M:%S UTC")


def ctime_utc(ts: int) -> str:
    """unix 초 → C ctime 형식 문자열 ('Fri Oct 30 14:20:10 2026', 일자는 공백 채움). 시간대 없이 UTC 로 만든다 (시험용)"""
    d = datetime.datetime(1970, 1, 1) + datetime.timedelta(seconds=ts)
    return f"{_WEEKDAYS[d.weekday()]} {_MONTHS[d.month - 1]} {d.day:2d} {d.hour:02d}:{d.minute:02d}:{d.second:02d} {d.year}"


def parse_ctime_utc(s: str):
    """'Www Mmm d HH:MM:SS YYYY' (영문, 공백 여러 개 가능) → 시간대 없이 UTC 로 본 unix 초. 못 읽으면 None. 로케일에 의존하지 않는다"""
    m = _CTIME_RE.fullmatch(" ".join(str(s).split()))
    if not m:
        return None
    wd, mon, day, hh, mm, ss, yy = m.groups()
    if wd.title() not in _WEEKDAYS or mon.title() not in _MONTHS:
        return None
    try:
        d = datetime.datetime(int(yy), _MONTHS.index(mon.title()) + 1, int(day), int(hh), int(mm), int(ss))
    except ValueError:
        return None
    return calendar.timegm(d.timetuple())


def pkg_safe_name(stem: str, ext: str, tail: str = "") -> str:
    """ZIP 항목 이름: 규칙 밖 문자는 _ 로, 앞뒤 ._ 제거, '..' 는 '.' 로, 전체 60자 이하. 결과는 PKG_NAME_RE 를 만족한다"""
    tail = re.sub(r"[^A-Za-z0-9._-]", "_", tail)
    s = re.sub(r"[^A-Za-z0-9._-]", "_", stem)
    s = re.sub(r"\.{2,}", ".", s).lstrip("._-")[:max(1, 60 - len(ext) - len(tail))].rstrip("._")
    return re.sub(r"\.{2,}", ".", (s or "app") + tail + ext)


# ── .out / .map 검사 ──

def read_out_info(data: bytes) -> dict:
    """.out 머리 검사 → {"format": coff1|coff2|elf, "target": int, "timestamp": int, "warnings": [..]}. 아니면 ValueError"""
    warns = []
    if data[:4] == b"\x7fELF":
        if len(data) < 20:
            raise ValueError("C2000 .out 이 아님 (ELF 머리가 잘림)")
        machine = struct.unpack_from("<H", data, 18)[0]
        if data[4] != 1 or data[5] != 1 or machine != ELF_MACHINE_C2000:
            raise ValueError(f"C2000 .out 이 아님 (ELF class {data[4]}, data {data[5]}, machine {machine}; "
                             f"32bit LE · machine {ELF_MACHINE_C2000} 이어야 함)")
        return {"format": "elf", "target": machine, "timestamp": 0, "warnings": warns}
    if len(data) >= 2 and struct.unpack_from("<H", data, 0)[0] in (0x00C1, 0x00C2):
        if len(data) < 22:
            raise ValueError("TI .out(COFF/ELF) 이 아님 (COFF 머리가 22바이트보다 짧음)")
        vid = struct.unpack_from("<H", data, 0)[0]
        ts = struct.unpack_from("<I", data, 4)[0]
        target = struct.unpack_from("<H", data, 20)[0]
        if target != COFF_TARGET_C2000:
            warns.append(f".out 의 COFF 대상 ID 가 0x{target:04X} (C2000 은 0x{COFF_TARGET_C2000:04X} 로 알고 있음) — C2000 용 .out 이 맞는지 확인")
        return {"format": "coff1" if vid == 0x00C1 else "coff2", "target": target, "timestamp": ts, "warnings": warns}
    raise ValueError("TI .out(COFF/ELF) 이 아님")


def read_map_info(data) -> dict:
    """CCS 링커 맵 검사 → {"output": 출력 파일 이름, "linked": 'Www Mmm d HH:MM:SS YYYY' 또는 ""}. bytes 또는 str"""
    if isinstance(data, (bytes, bytearray)):
        if b"\0" in data:
            raise ValueError("CCS 링커 맵이 아님 (NUL 바이트가 있음)")
        text = bytes(data).decode("latin-1")
    else:
        text = data
    m = re.search(r"OUTPUT FILE NAME:\s*<([^>]+)>", text)
    out = _basename(m.group(1).strip()) if m else ""
    if not out:
        raise ValueError("CCS 링커 맵이 아님 (OUTPUT FILE NAME 이 없음)")
    linked = ""
    lm = re.search(r"^>> Linked (.+)$", text, re.M)
    if lm:
        s = " ".join(lm.group(1).split())
        if parse_ctime_utc(s) is not None:      # 못 읽으면 없는 것으로
            linked = s
    return {"output": out, "linked": linked}


def link_check(out_info: dict, map_info: dict):
    """.map ↔ .out 연결 확인 (규칙은 패커·게이트웨이 공통). 통과하면 ("time" | "name", 경고 목록), 아니면 ValueError.
    out_info: timestamp, orig_name(없으면 name)  map_info: output, linked(문자열)"""
    ts = out_info.get("timestamp") or 0
    lt = parse_ctime_utc(map_info.get("linked") or "")
    out_name = out_info.get("orig_name") or out_info.get("name") or ""
    map_out = map_info.get("output") or ""
    same_name = map_out.lower() == out_name.lower()
    if ts > 0 and lt is not None:
        d = ts - lt                  # 링크 PC 의 시간대 오프셋만큼 차이 남
        if not any(abs(d - LINK_TZ_STEP * k) <= LINK_TOL_SEC for k in range(-LINK_TZ_MAX_K, LINK_TZ_MAX_K + 1)):
            raise ValueError(f"map 과 .out 의 링크 시각이 다름 — 같은 빌드의 .map 이 아님 "
                             f"(.out {_utc_str(ts)}, map {map_info.get('linked')}, 차이 {d} 초)")
        warns = [] if same_name else [f"map 의 출력 파일 이름({map_out})과 .out 이름({out_name})이 다르지만 링크 시각이 맞아 통과"]
        return "time", warns
    if not same_name:
        raise ValueError(f"map 의 출력 파일 이름({map_out})과 .out 이름({out_name})이 다름")
    return "name", ["링크 시각을 비교하지 못해(ELF .out 이거나 map 에 Linked 줄이 없음) 파일 이름만 비교함"]


# ── hex2000 ──

def _hex2000_exe(p: str):
    """실행 파일 경로 확인. .py 는 시험용(가짜 hex2000)으로 그대로, 확장자 없는 경로(Windows .exe)·PATH 이름은 which 로"""
    if p.lower().endswith(".py"):
        return os.path.abspath(p) if os.path.isfile(p) else None
    if os.path.isfile(p):
        return os.path.abspath(p)
    return shutil.which(p)


def _hex2000_cmd(path: str) -> list:
    return [sys.executable, path] if path.lower().endswith(".py") else [path]


def find_hex2000(explicit=None) -> str:
    """hex2000 경로. 순서: --hex2000 → 환경변수 HEX2000 → PATH → Windows C:\\ti\\ccs*\\ccs\\tools\\compiler\\ti-cgt-c2000_*\\bin 중 가장 새 것"""
    if explicit:
        p = _hex2000_exe(explicit)
        if not p:
            raise ValueError(f"--hex2000 파일이 없음: {explicit}")
        return p
    tried = []
    env = os.environ.get("HEX2000")
    if env:
        p = _hex2000_exe(env)
        if p:
            return p
        tried.append(f"환경변수 HEX2000={env} (없음)")
    p = shutil.which("hex2000")
    if p:
        return p
    tried.append("PATH 의 hex2000 (없음)")
    if os.name == "nt":
        pat = "C:/ti/ccs*/ccs/tools/compiler/ti-cgt-c2000_*/bin/hex2000.exe"
        found = glob.glob(pat)
        ver = lambda s: tuple(int(x) for x in re.findall(r"\d+", s))
        if found:
            # 컴파일러 버전 → CCS 버전 순으로 가장 새 것
            return max(found, key=lambda f: (ver(os.path.basename(os.path.dirname(os.path.dirname(f)))), ver(f), f))
        tried.append(f"{pat} (없음)")
    raise ValueError("hex2000 을 찾지 못함 — 찾은 곳: " + "; ".join(tried) +
                     ". --hex2000 <경로> 로 지정 (CCS: \"${CG_TOOL_ROOT}/bin/hex2000\")")


def run_hex2000(hex2000: str, out_path: str, hex_path: str):
    """hex2000 -i -o <hex> <out> --memwidth=16 --romwidth=16 (기존 post-build 와 같은 옵션). 종료 코드·HEX 생성 확인"""
    cmd = _hex2000_cmd(hex2000) + ["-i", "-o", hex_path, os.path.abspath(out_path), "--memwidth=16", "--romwidth=16"]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=HEX2000_TIMEOUT)
    except OSError as e:
        raise ValueError(f"hex2000 을 실행하지 못함 ({hex2000}): {e}")
    except subprocess.TimeoutExpired:
        raise ValueError(f"hex2000 이 {HEX2000_TIMEOUT}초 안에 끝나지 않음")
    tail = r.stdout.decode("utf-8", "replace").strip()[-600:]
    if r.returncode != 0:
        raise ValueError(f"hex2000 실패 (종료 코드 {r.returncode})" + (f": {tail}" if tail else ""))
    if not os.path.isfile(hex_path) or os.path.getsize(hex_path) == 0:
        raise ValueError("hex2000 이 오류 없이 끝났지만 HEX 파일을 만들지 않음" + (f": {tail}" if tail else ""))


# ── 만들기 ──

def _json_bytes(obj) -> bytes:
    return (json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def _link_mode(out_data: bytes, out_orig: str, map_data: bytes, map_orig: str, no_link_check: bool):
    """형식 검사 + 연결 확인 → (out_info, map_info, 모드)"""
    out_orig, map_orig = _basename(out_orig), _basename(map_orig)
    try:
        oi = read_out_info(out_data)
    except ValueError as e:
        raise ValueError(f"{out_orig}: {e}")
    try:
        mi = read_map_info(map_data)
    except ValueError as e:
        raise ValueError(f"{map_orig}: {e}")
    if no_link_check:
        return oi, mi, "skipped"
    mode, _ = link_check({"timestamp": oi["timestamp"], "orig_name": out_orig}, mi)
    return oi, mi, mode


def make_package(dspfw: bytes, out_data: bytes, out_orig: str, map_data: bytes, map_orig: str,
                 hex2000: str = "", no_link_check: bool = False, created=None):
    """.dspfw + .out + .map → (.dsppkg 바이트, verify_package 결과). 만든 직후 읽는 쪽 규칙으로 다시 검증한다"""
    info = unpack(dspfw)
    ver = info["ver"]
    out_orig, map_orig = _basename(out_orig), _basename(map_orig)
    oi, mi, mode = _link_mode(out_data, out_orig, map_data, map_orig, no_link_check)
    created = int(time.time()) if created is None else int(created)
    out_name = pkg_safe_name(os.path.splitext(out_orig)[0], ".out")
    map_name = pkg_safe_name(os.path.splitext(map_orig)[0], ".map")
    dsp_name = pkg_safe_name(os.path.splitext(out_orig)[0], ".dspfw", "_" + ver)
    sha = lambda b: hashlib.sha256(b).hexdigest()
    manifest = {
        "format": PKG_FORMAT, "format_ver": PKG_FORMAT_VER, "ver": ver,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(created)),
        "tool": "dsp_pack.py bundle", "hex2000": hex2000, "link_check": mode,
        "files": {
            "dspfw": {"name": dsp_name, "size": len(dspfw), "sha256": sha(dspfw)},
            "out": {"name": out_name, "orig_name": out_orig, "size": len(out_data), "sha256": sha(out_data),
                    "format": oi["format"], "target": oi["target"], "timestamp": oi["timestamp"]},
            "map": {"name": map_name, "orig_name": map_orig, "size": len(map_data), "sha256": sha(map_data),
                    "output": mi["output"], "linked": mi["linked"]},
        },
        "dspfw": {"app_len": info["app_len"], "app_crc32": f"{info['app_crc']:08x}", "build_time": info["build_time"]},
    }
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in ((PKG_MANIFEST, _json_bytes(manifest)), (dsp_name, dspfw), (out_name, out_data), (map_name, map_data)):
            zi = zipfile.ZipInfo(name, time.gmtime(created)[:6])
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 3
            zi.external_attr = 0o644 << 16
            zf.writestr(zi, data, compresslevel=9)
    pkg = buf.getvalue()
    return pkg, verify_package(pkg)


def bundle(out_path: str, map_path: str, ver: str, output=None, hex2000=None, build=None,
           no_link_check: bool = False, created=None) -> dict:
    """app.out + app.map → .dsppkg. .dspfw 는 반드시 이 .out 에서 hex2000 으로 만든다. verify_package 결과에 "path" 를 더해 돌려준다"""
    check_ver(ver)
    sizes = {}
    for key, path, label in (("out", out_path, ".out"), ("map", map_path, ".map")):
        sizes[key] = os.path.getsize(path)
        if sizes[key] > PKG_LIMITS[key]:
            raise ValueError(f"{label} 이 한도({PKG_LIMITS[key] >> 20} MiB)를 넘음: {path}")
    with open(out_path, "rb") as f:
        out_data = f.read()
    with open(map_path, "rb") as f:
        map_data = f.read()
    _link_mode(out_data, out_path, map_data, map_path, no_link_check)   # 1. 형식·연결 확인 (hex2000 을 돌리기 전에)
    hex2000 = find_hex2000(hex2000)                                 # 2. hex2000 → HEX
    with tempfile.TemporaryDirectory() as d:
        hex_path = os.path.join(d, "app.hex")
        run_hex2000(hex2000, out_path, hex_path)
        dspfw = pack(parse_intel_hex(hex_path), ver, int(time.time()) if build is None else int(build))   # 3. .dspfw
    pkg, res = make_package(dspfw, out_data, out_path, map_data, map_path, hex2000, no_link_check, created)   # 4. ZIP
    if output is None:
        output = os.path.join(os.path.dirname(os.path.abspath(out_path)),
                              pkg_safe_name(os.path.splitext(_basename(out_path))[0], ".dsppkg", "_" + ver))
    with open(output, "wb") as f:
        f.write(pkg)
    res["path"] = output
    res["pkg_size"] = len(pkg)
    return res


# ── 읽기·검증 ──

def sniff_package(path) -> bool:
    """파일 머리가 ZIP 이면 패키지로 본다 (.dspfw 는 'AGVDSP1' 로 시작)"""
    with open(path, "rb") as f:
        return f.read(4) in (b"PK\x03\x04", b"PK\x05\x06")


def _pkg_entries(infos) -> dict:
    """ZIP 항목 이름·종류 검사 (읽기 전). 정확히 manifest.json / .dspfw / .out / .map 하나씩 → {종류: ZipInfo}"""
    if len(infos) > 16:
        raise ValueError(f"패키지 항목이 너무 많음 ({len(infos)}개) — manifest.json, .dspfw, .out, .map 4개여야 함")
    kinds = {}
    seen = set()
    for zi in infos:
        n = zi.filename
        if n.endswith("/"):
            raise ValueError(f"디렉터리 항목은 쓸 수 없음: {n!r}")
        if "/" in n or "\\" in n or ".." in n or PKG_NAME_RE.fullmatch(n) is None:
            raise ValueError(f"항목 이름이 규칙에 어긋남: {n!r} (영문·숫자·. _ - 만, 60자 이하, 경로·.. 금지)")
        if n.casefold() in seen:
            raise ValueError(f"항목 이름이 중복됨: {n!r}")
        seen.add(n.casefold())
        if n == PKG_MANIFEST:
            kind = "manifest"
        else:
            kind = {".dspfw": "dspfw", ".out": "out", ".map": "map"}.get(os.path.splitext(n)[1].lower())
            if kind is None:
                raise ValueError(f"허용되지 않는 항목: {n!r} — manifest.json, .dspfw, .out, .map 만 들어갈 수 있음")
        if kind in kinds:
            raise ValueError(f"{PKG_KIND_LABEL[kind]} 항목이 둘 이상: {kinds[kind].filename!r}, {n!r}")
        if zi.flag_bits & 0x1:
            raise ValueError(f"암호화된 항목은 쓸 수 없음: {n!r}")
        if zi.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            raise ValueError(f"압축 방식을 지원하지 않음 (stored·deflate 만): {n!r}")
        kinds[kind] = zi
    missing = [PKG_KIND_LABEL[k] for k in ("manifest", "dspfw", "out", "map") if k not in kinds]
    if missing:
        raise ValueError("패키지에 빠진 항목: " + ", ".join(missing) + " (manifest.json, .dspfw, .out, .map 이 모두 있어야 함)")
    return kinds


def _read_limited(zf, zi, limit: int, label: str) -> bytes:
    """ZIP 이 선언한 크기를 믿지 않고 실제로 읽으면서 한도를 넘으면 중단"""
    chunks, total = [], 0
    try:
        with zf.open(zi) as f:
            while True:
                b = f.read(min(1 << 20, limit + 1 - total))
                if not b:
                    break
                total += len(b)
                if total > limit:
                    raise ValueError(f"{label} 이(가) 크기 한도 {limit:,} B 를 넘음 ({zi.filename!r}, 압축 해제 중 중단)")
                chunks.append(b)
    except (zipfile.BadZipFile, zlib.error, EOFError, NotImplementedError, RuntimeError) as e:
        raise ValueError(f"{label} 을(를) 읽지 못함 ({zi.filename!r}): {e}")
    return b"".join(chunks)


def _need(d, key, typ, where=""):
    """manifest 값 꺼내기 (없거나 형이 다르면 ValueError). int 는 bool 을 받지 않는다"""
    if not isinstance(d, dict) or key not in d:
        raise ValueError(f"manifest 에 {where}{key} 가 없음")
    v = d[key]
    if (typ is int and type(v) is not int) or (typ is not int and not isinstance(v, typ)):
        raise ValueError(f"manifest 의 {where}{key} 형식이 올바르지 않음 ({'정수' if typ is int else typ.__name__} 이어야 함)")
    return v


def verify_package(src, limits=None) -> dict:
    """.dsppkg 전체 검증 (규격 1~4절). src: 경로 또는 bytes. limits: 종류별 크기 한도 덮어쓰기 (시험용).
    통과하면 {pkg_id, ver, link_check, files, sizes, dspfw_bytes, dspfw_info, out, map, warnings, ...}, 아니면 ValueError"""
    lim = dict(PKG_LIMITS)
    lim.update(limits or {})
    try:
        zf = zipfile.ZipFile(io.BytesIO(bytes(src)) if isinstance(src, (bytes, bytearray)) else src)
    except zipfile.BadZipFile as e:
        raise ValueError(f"패키지(.dsppkg, ZIP) 형식이 아님: {e}")
    with zf:
        entries = _pkg_entries(zf.infolist())
        data = {k: _read_limited(zf, entries[k], lim[k], PKG_KIND_LABEL[k]) for k in ("manifest", "dspfw", "out", "map")}
    warns = []

    # manifest
    try:
        mf = json.loads(data["manifest"].decode("utf-8"))
    except ValueError as e:         # UnicodeDecodeError·JSONDecodeError 포함
        raise ValueError(f"manifest.json 을 읽지 못함: {e}")
    if not isinstance(mf, dict):
        raise ValueError("manifest.json 이 JSON 객체가 아님")
    if mf.get("format") != PKG_FORMAT or type(mf.get("format_ver")) is not int or mf["format_ver"] != PKG_FORMAT_VER:
        raise ValueError(f"패키지 형식이 아님 (format {mf.get('format')!r}, format_ver {mf.get('format_ver')!r}; "
                         f"{PKG_FORMAT!r} · {PKG_FORMAT_VER} 이어야 함)")
    ver = _need(mf, "ver", str)
    try:
        check_ver(ver)
    except ValueError as e:
        raise ValueError(f"manifest 의 {e}")
    files = _need(mf, "files", dict)
    info_str = {k: (mf[k] if isinstance(mf.get(k), str) else "") for k in ("created_utc", "tool", "hex2000")}

    # 항목 이름·크기·해시
    ents, sha = {}, {}
    for kind in ("dspfw", "out", "map"):
        w = f"files.{kind}."
        e = ents[kind] = _need(files, kind, dict, "files.")
        name = _need(e, "name", str, w)
        if name != entries[kind].filename:
            raise ValueError(f"manifest 의 {w}name({name!r}) 이 ZIP 항목({entries[kind].filename!r})과 다름")
        if _need(e, "size", int, w) != len(data[kind]):
            raise ValueError(f"manifest 의 {w}size({e['size']}) 가 실제 크기({len(data[kind])})와 다름 ({name})")
        sha[kind] = hashlib.sha256(data[kind]).hexdigest()
        if _need(e, "sha256", str, w).lower() != sha[kind]:
            raise ValueError(f"manifest 의 {w}sha256 이 실제 내용과 다름 — 변조 또는 손상 ({name})")
    for kind in ("out", "map"):
        o = ents[kind].get("orig_name", ents[kind]["name"])
        if not isinstance(o, str) or not o or len(o) > 255 or any(ord(c) < 32 for c in o):
            raise ValueError(f"manifest 의 files.{kind}.orig_name 이 올바르지 않음")

    # .dspfw ↔ manifest
    try:
        di = unpack(data["dspfw"])
    except ValueError as e:
        raise ValueError(f".dspfw 가 올바르지 않음: {e}")
    fver = data["dspfw"][16:32].split(b"\0", 1)[0].decode("ascii", "replace")
    if fver != di["ver"]:
        raise ValueError(f".dspfw 의 파일 헤더 버전({fver})과 앱 헤더 버전({di['ver']})이 다름")
    if ver != di["ver"]:
        raise ValueError(f"manifest 의 ver({ver}) 과 .dspfw 헤더 버전({di['ver']})이 다름")
    md = _need(mf, "dspfw", dict)
    if _need(md, "app_len", int, "dspfw.") != di["app_len"]:
        raise ValueError(f"manifest 의 dspfw.app_len({md['app_len']}) 이 .dspfw 헤더({di['app_len']})와 다름")
    if _need(md, "app_crc32", str, "dspfw.").lower() != f"{di['app_crc']:08x}":
        raise ValueError(f"manifest 의 dspfw.app_crc32({md['app_crc32']}) 가 .dspfw 헤더({di['app_crc']:08x})와 다름")
    if _need(md, "build_time", int, "dspfw.") != di["build_time"]:
        raise ValueError(f"manifest 의 dspfw.build_time({md['build_time']}) 이 .dspfw 헤더({di['build_time']})와 다름")

    # .out / .map ↔ manifest
    try:
        oi = read_out_info(data["out"])
    except ValueError as e:
        raise ValueError(f"{entries['out'].filename}: {e}")
    try:
        mi = read_map_info(data["map"])
    except ValueError as e:
        raise ValueError(f"{entries['map'].filename}: {e}")
    warns += oi["warnings"]
    eo, em = ents["out"], ents["map"]
    for k, typ in (("format", str), ("target", int), ("timestamp", int)):
        if _need(eo, k, typ, "files.out.") != oi[k]:
            raise ValueError(f"manifest 의 files.out.{k}({eo[k]}) 가 .out 에서 읽은 값({oi[k]})과 다름")
    for k in ("output", "linked"):
        if _need(em, k, str, "files.map.") != mi[k]:
            raise ValueError(f"manifest 의 files.map.{k}({em[k]!r}) 가 .map 에서 읽은 값({mi[k]!r})과 다름")

    # 연결 확인 (skipped 가 아니면 같은 규칙을 다시 돌린다)
    mode = _need(mf, "link_check", str)
    if mode not in PKG_LINK_MODES:
        raise ValueError(f"manifest 의 link_check({mode!r}) 가 올바르지 않음 ({'/'.join(PKG_LINK_MODES)})")
    if mode == "skipped":
        warns.append("link_check=skipped — .map 과 .out 의 연결이 확인되지 않은 패키지 (--no-link-check 로 만듦)")
    else:
        got, w = link_check({"timestamp": oi["timestamp"], "name": entries["out"].filename, "orig_name": eo.get("orig_name")}, mi)
        if got != mode:
            raise ValueError(f"manifest 의 link_check({mode}) 가 다시 계산한 값({got})과 다름")
        warns += w

    return {
        "pkg_id": hashlib.sha256(data["manifest"]).hexdigest()[:16],
        "ver": ver, "link_check": mode, "manifest": mf, **info_str,
        "files": ents, "sha256": sha, "sizes": {k: len(v) for k, v in data.items()},
        "out": oi, "map": mi, "dspfw_info": di, "dspfw_bytes": data["dspfw"], "warnings": warns,
    }


def load_dspfw_info(path: str):
    """.dspfw 또는 .dsppkg 경로 → (unpack 결과, 패키지 검증 결과 또는 None). 패키지는 전체 검증 후 안의 .dspfw 를 쓴다"""
    if sniff_package(path):
        res = verify_package(path)
        return res["dspfw_info"], res
    with open(path, "rb") as f:
        return unpack(f.read()), None


# ───────── 시험용 도우미 (selftest, tests/host/test_usb_stage.py) ─────────

def synth_coff_out(ts: int, target: int = COFF_TARGET_C2000, vid: int = 0x00C2, pad: int = 64) -> bytes:
    """합성 TI COFF .out: 22바이트 머리(시각·대상 ID) + 채움"""
    return struct.pack("<HHIIIHHH", vid, 1, ts, 0, 0, 0, 0, target) + bytes(range(256))[:pad]


def synth_elf_out(machine: int = ELF_MACHINE_C2000, pad: int = 64) -> bytes:
    """합성 ELF32 LE .out: 52바이트 머리 + 채움"""
    h = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8) + struct.pack("<HHI", 2, machine, 1) + bytes(32)
    return h + bytes(pad)


def synth_map(out_name: str, linked=None) -> bytes:
    """합성 CCS 링커 맵. linked: 'Www Mmm d HH:MM:SS YYYY' 또는 None(Linked 줄 없음)"""
    s = "*" * 78 + "\n            TMS320C2000 Linker PC v18.12.4\n" + "*" * 78 + "\n"
    if linked:
        s += f">> Linked {linked}\n"
    s += f"\nOUTPUT FILE NAME:   <./Debug/{out_name}>\nENTRY POINT SYMBOL: \"_c_int00\"  address: 003e8040\n"
    return s.encode("latin-1")


def make_test_hex(mem: dict) -> str:
    """워드 주소 → 워드 값 사전을 hex2000 --memwidth=16 --romwidth=16 모양의 Intel HEX 로"""
    def rec(addr, typ, data):
        b = bytes([len(data), addr >> 8, addr & 0xFF, typ]) + data
        return ":" + (b + bytes([(-sum(b)) & 0xFF])).hex().upper()
    lines, base, addrs, i = [], None, sorted(mem), 0
    while i < len(addrs):
        a = addrs[i]
        if a >> 16 != base:
            base = a >> 16
            lines.append(rec(0, 4, bytes([base >> 8, base & 0xFF])))
        j, data = i, b""
        while j < len(addrs) and addrs[j] == a + (j - i) and addrs[j] >> 16 == base and j - i < 8:
            data += bytes([mem[addrs[j]] >> 8, mem[addrs[j]] & 0xFF])
            j += 1
        lines.append(rec(a & 0xFFFF, 0, data))
        i = j
    return "\n".join(lines + [":00000001FF"]) + "\n"


_FAKE_HEX2000 = '''\
import shutil, sys
# 시험용 가짜 hex2000: dsp_pack.py bundle 이 부르는 인자 모양을 확인하고 정해 둔 HEX 를 -o 위치로 복사한다
a = sys.argv[1:]
if len(a) != 6 or a[:2] != ["-i", "-o"] or a[4:] != ["--memwidth=16", "--romwidth=16"]:
    sys.stderr.write("fake hex2000: bad args %r\\n" % (a,))
    sys.exit(2)
open(a[3], "rb").close()                    # .out 입력이 있어야 함
sys.stdout.write("fake hex2000 ran\\n")
if @WRITE@:
    shutil.copyfile(@SRC@, a[2])
sys.exit(@RC@)
'''


def write_fake_hex2000(dirpath: str, src_hex: str, rc: int = 0, write: bool = True, name: str = "fake_hex2000.py") -> str:
    """시험용 가짜 hex2000 스크립트를 만들고 경로를 돌려준다. bundle 은 --hex2000 이 .py 면 sys.executable 로 실행한다"""
    path = os.path.join(dirpath, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(_FAKE_HEX2000.replace("@SRC@", repr(os.path.abspath(src_hex))).replace("@RC@", str(rc)).replace("@WRITE@", repr(write)))
    return path


# ───────── 명령 ─────────

def cmd_pack(a):
    mem = parse_intel_hex(a.hexfile)
    build = int(a.build) if a.build else int(time.time())
    out = pack(mem, a.ver, build)
    with open(a.output, "wb") as f:
        f.write(out)
    print(f"wrote {a.output}", file=sys.stderr)


def print_dspfw_info(info: dict):
    words = sum(len(ws) for _, ws in info["blocks"])
    print(f"ver        : {info['ver']}")
    print(f"build_time : {info['build_time']} ({time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(info['build_time']))})")
    print(f"app_len    : {info['app_len']} words ({info['app_len'] * 2} B), end 0x{APP_CODE_ADDR + info['app_len'] - 1:X}")
    print(f"app_crc32  : {info['app_crc']:08X}")
    print(f"blocks     : {len(info['blocks'])} ({words} words transferred)")


def print_package_info(res: dict):
    fs, o, m = res["files"], res["out"], res["map"]
    print(f"package    : pkg id {res['pkg_id']}, ver {res['ver']}, link_check {res['link_check']}")
    print(f"created    : {res['created_utc']} ({res['tool']})")
    print(f"hex2000    : {res['hex2000']}")
    ts = f", linked {_utc_str(o['timestamp'])}" if o["timestamp"] else ""
    print(f"out        : {fs['out']['name']} {res['sizes']['out']} B, {o['format']} target 0x{o['target']:X}{ts}")
    print(f"             orig {fs['out'].get('orig_name', fs['out']['name'])}, sha256 {res['sha256']['out']}")
    print(f"map        : {fs['map']['name']} {res['sizes']['map']} B, output {m['output']}" + (f", linked {m['linked']}" if m["linked"] else ""))
    print(f"             orig {fs['map'].get('orig_name', fs['map']['name'])}, sha256 {res['sha256']['map']}")
    print(f"dspfw      : {fs['dspfw']['name']} {res['sizes']['dspfw']} B, sha256 {res['sha256']['dspfw']}")
    for w in res["warnings"]:
        print(f"warning: {w}", file=sys.stderr)


def cmd_info(a):
    if sniff_package(a.file):
        res = verify_package(a.file)          # 읽으면서 전체 검증 — 통과해야 표시
        print_package_info(res)
        print("-- dspfw --")
        print_dspfw_info(res["dspfw_info"])
        return
    with open(a.file, "rb") as f:
        info = unpack(f.read())
    print_dspfw_info(info)


def cmd_bundle(a):
    res = bundle(a.outfile, a.map, a.ver, a.output, a.hex2000, int(a.build) if a.build else None, a.no_link_check)
    s = res["sizes"]
    o = res["out"]
    print(f"package: {res['path']} ({res['pkg_size']} B)", file=sys.stderr)
    print(f"  ver {res['ver']}, pkg id {res['pkg_id']}, link_check {res['link_check']}", file=sys.stderr)
    print(f"  dspfw {s['dspfw']} B, out {s['out']} B ({o['format']}" + (f", {_utc_str(o['timestamp'])}" if o["timestamp"] else "") +
          f"), map {s['map']} B", file=sys.stderr)
    for w in res["warnings"]:
        print(f"warning: {w}", file=sys.stderr)


def cmd_verify(a):
    res = verify_package(a.file)
    for w in res["warnings"]:
        print(f"warning: {w}", file=sys.stderr)
    print(f"ok: {os.path.basename(a.file)} ver {res['ver']}, pkg id {res['pkg_id']}, link_check {res['link_check']}")


def selftest_package():
    """패키지 시험 — TI 도구 없이: 가짜 hex2000 + 합성 COFF/ELF .out + 합성 .map"""
    ts = 1793422810                                   # .out 링크 시각 (임의)
    mem = {APP_CODE_ADDR + i: (i * 7 + 1) & 0xFFFF for i in range(300)}
    quiet = lambda: contextlib.redirect_stderr(io.StringIO())

    def fails(fn, text):
        try:
            fn()
        except ValueError as e:
            assert text in str(e), f"{text!r} 가 메시지에 없음: {e}"
            return
        raise AssertionError(f"실패해야 함 ({text})")

    # ctime 형식: C asctime 과 같고 로케일에 의존하지 않음
    for t in (0, 86400 * 3 + 3661, ts, 951782400 + 86399):
        assert ctime_utc(t) == time.asctime(time.gmtime(t)), (t, ctime_utc(t))
        assert parse_ctime_utc(ctime_utc(t)) == t
    assert parse_ctime_utc("Mon  Oct   5 01:02:03   2026") == parse_ctime_utc("Mon Oct 5 01:02:03 2026") is not None
    assert parse_ctime_utc("Foo Oct 5 01:02:03 2026") is None and parse_ctime_utc("Mon Oct 32 01:02:03 2026") is None
    # 항목 이름 규칙
    for raw, ext, tail in (("AGV Main", ".out", ""), ("펌웨어 v2..final (1)", ".map", ""), ("-x", ".out", ""), (".hidden", ".map", ""),
                           ("a" * 100, ".out", ""), ("..", ".out", ""), ("AGV_Main", ".dspfw", "_1.2.3"), ("a" * 100, ".dspfw", "_1..2")):
        n = pkg_safe_name(raw, ext, tail)
        assert PKG_NAME_RE.fullmatch(n) and ".." not in n and n.endswith(ext), (raw, n)
    assert pkg_safe_name("AGV Main", ".out") == "AGV_Main.out" and pkg_safe_name("펌웨어 v2..final (1)", ".map") == "v2.final__1.map"

    with tempfile.TemporaryDirectory() as d:
        P = lambda n: os.path.join(d, n)

        def W(name, data):
            with open(P(name), "wb") as f:
                f.write(data)
            return P(name)

        def R(name):
            with open(P(name), "rb") as f:
                return f.read()

        W("app.hex", make_test_hex(mem).encode("ascii"))
        fake = write_fake_hex2000(d, P("app.hex"))

        def mk(out, mp, **kw):
            with quiet():
                return bundle(W("app.out", out), W("app.map", mp), kw.pop("ver", "1.2.3"), P("t.dsppkg"), kw.pop("hex2000", fake),
                              build=1700000000, created=1793423000, **kw)

        # 1. 정상: 링크 시각 규칙, 가짜 hex2000 → 패키지 → 다시 읽기
        r = mk(synth_coff_out(ts), synth_map("app.out", ctime_utc(ts)))
        assert r["link_check"] == "time" and r["warnings"] == [] and r["path"] == P("t.dsppkg"), r["warnings"]
        with quiet():
            want = pack(parse_intel_hex(P("app.hex")), "1.2.3", 1700000000)
        assert r["dspfw_bytes"] == want
        good = R("t.dsppkg")
        with zipfile.ZipFile(io.BytesIO(good)) as z:
            assert z.namelist() == ["manifest.json", "app_1.2.3.dspfw", "app.out", "app.map"], z.namelist()
            mbytes = z.read("manifest.json")
        assert mbytes.endswith(b"\n") and mbytes == _json_bytes(json.loads(mbytes)), "manifest 는 sort_keys·indent=2·끝 줄바꿈"
        v = verify_package(P("t.dsppkg"))
        assert v["pkg_id"] == hashlib.sha256(mbytes).hexdigest()[:16] == r["pkg_id"] and v["dspfw_bytes"] == want
        assert v["ver"] == "1.2.3" and v["out"]["format"] == "coff2" and v["map"]["output"] == "app.out"
        assert verify_package(good)["pkg_id"] == v["pkg_id"] and sniff_package(P("t.dsppkg")) and not sniff_package(P("app.hex"))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            print_package_info(v)
        assert v["pkg_id"] in buf.getvalue() and "coff2" in buf.getvalue()
        info, res = load_dspfw_info(P("t.dsppkg"))
        assert res["pkg_id"] == v["pkg_id"] and info["ver"] == "1.2.3" and load_dspfw_info(W("w.dspfw", want))[1] is None
        # 같은 입력이면 같은 패키지 (재현성)
        assert mk(synth_coff_out(ts), synth_map("app.out", ctime_utc(ts)))["pkg_id"] == r["pkg_id"]
        # 시험용 .py hex2000 은 환경변수 HEX2000 으로도 찾음
        old = os.environ.get("HEX2000")
        os.environ["HEX2000"] = fake
        try:
            assert find_hex2000() == fake
        finally:
            os.environ.pop("HEX2000") if old is None else os.environ.__setitem__("HEX2000", old)
        fails(lambda: find_hex2000(P("none.py")), "--hex2000 파일이 없음")

        # 2. 링크 시각: 시간대 격자 (+9시간, 네팔 +5:45), 허용 오차 ±5초
        for off in (0, 9 * 3600, -5 * 3600, 20700, 9 * 3600 - 5, 9 * 3600 + 5, 14 * 3600):
            assert mk(synth_coff_out(ts), synth_map("app.out", ctime_utc(ts + off)))["link_check"] == "time", off
        for off in (9 * 3600 + 6, 1000, 3 * 3600 + 420, 14 * 3600 + 900, -14 * 3600 - 900):
            fails(lambda: mk(synth_coff_out(ts), synth_map("app.out", ctime_utc(ts + off))), "링크 시각이 다름")
        # 이름이 달라도 시각이 맞으면 통과 (경고만)
        r = mk(synth_coff_out(ts), synth_map("other.out", ctime_utc(ts + 9 * 3600)))
        assert r["link_check"] == "time" and any("링크 시각이 맞아 통과" in w for w in r["warnings"]), r["warnings"]

        # 3. 이름 규칙: 시각을 못 쓸 때 (ELF, Linked 줄 없음, 시각 0)
        for out, mp in ((synth_elf_out(), synth_map("app.out")), (synth_elf_out(), synth_map("APP.OUT", ctime_utc(ts))),
                        (synth_coff_out(ts), synth_map("app.out")), (synth_coff_out(0), synth_map("app.out", ctime_utc(ts)))):
            r = mk(out, mp)
            assert r["link_check"] == "name" and r["warnings"], r["link_check"]
        assert mk(synth_elf_out(), synth_map("app.out"))["out"]["format"] == "elf"
        fails(lambda: mk(synth_elf_out(), synth_map("other.out")), "other.out")
        fails(lambda: mk(synth_coff_out(ts), synth_map("other.out")), "app.out")
        # 시각을 못 읽는 Linked 줄은 없는 것으로
        assert read_map_info(synth_map("a.out", "not a time"))["linked"] == ""
        assert read_map_info((synth_map("a.out", ctime_utc(ts)).decode().replace("\n", "\r\n")).encode())["linked"] == ctime_utc(ts)

        # 4. --no-link-check: 맞지 않아도 만들고 skipped 로 기록, 경고
        r = mk(synth_coff_out(ts), synth_map("other.out", ctime_utc(ts + 1000)), no_link_check=True)
        assert r["link_check"] == "skipped" and any("skipped" in w for w in r["warnings"])
        assert verify_package(R("t.dsppkg"))["link_check"] == "skipped"

        # 5. .out / .map 형식
        fails(lambda: mk(synth_elf_out(machine=40), synth_map("app.out")), "C2000 .out 이 아님")
        fails(lambda: mk(b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) + struct.pack("<HH", 2, 141) + bytes(40), synth_map("app.out")), "C2000 .out 이 아님")
        fails(lambda: mk(b"\x7fELF\x01\x01", synth_map("app.out")), "C2000 .out 이 아님")
        for bad in (b"hello world " * 10, b"MZ\x90\x00" + bytes(100), b"", b"\xc2\x00" + bytes(10)):
            fails(lambda: mk(bad, synth_map("app.out")), "TI .out(COFF/ELF) 이 아님")
        fails(lambda: mk(synth_coff_out(ts), b"just some text\n"), "CCS 링커 맵이 아님")
        fails(lambda: mk(synth_coff_out(ts), synth_map("app.out") + b"\0\1\2"), "CCS 링커 맵이 아님")
        r = mk(synth_coff_out(ts, target=0x28), synth_map("app.out", ctime_utc(ts)))
        assert r["out"]["target"] == 0x28 and any("대상 ID" in w for w in r["warnings"]), "COFF 대상 ID 가 달라도 경고만"
        assert mk(synth_coff_out(ts, vid=0x00C1), synth_map("app.out", ctime_utc(ts)))["out"]["format"] == "coff1"
        # 이름이 규칙 밖이면 바꾸고 원래 이름은 orig_name 에 남김. 둘 다 같은 이름이라 링크 확인은 원래 이름으로
        with quiet():
            r = bundle(W("AGV Main (v2).out", synth_coff_out(ts)), W("AGV Main (v2).map", synth_map("AGV Main (v2).out")), "1.2.3",
                       P("n.dsppkg"), fake, build=1700000000)
        assert r["link_check"] == "name", r["link_check"]
        assert (r["files"]["out"]["name"], r["files"]["out"]["orig_name"]) == ("AGV_Main__v2.out", "AGV Main (v2).out"), r["files"]["out"]
        assert (r["files"]["map"]["name"], r["files"]["map"]["orig_name"]) == ("AGV_Main__v2.map", "AGV Main (v2).map")
        assert r["files"]["dspfw"]["name"] == "AGV_Main__v2_1.2.3.dspfw", r["files"]["dspfw"]["name"]

        # 6. hex2000 오류와 입력 검사
        fails(lambda: mk(synth_coff_out(ts), synth_map("app.out"), hex2000=write_fake_hex2000(d, P("app.hex"), rc=7, name="f7.py")), "종료 코드 7")
        fails(lambda: mk(synth_coff_out(ts), synth_map("app.out"), hex2000=write_fake_hex2000(d, P("app.hex"), write=False, name="fn.py")),
              "HEX 파일을 만들지 않음")
        fails(lambda: mk(synth_coff_out(ts), synth_map("app.out"), ver="1.2.3-rc1"), "버전 문자열")
        fails(lambda: mk(synth_coff_out(ts), synth_map("app.out"), hex2000=P("nothing.py")), "--hex2000 파일이 없음")
        W("bad.hex", b":00000001FF\n")           # 데이터 없는 HEX
        fails(lambda: mk(synth_coff_out(ts), synth_map("app.out"), hex2000=write_fake_hex2000(d, P("bad.hex"), name="fb.py")), "데이터가 없음")
        r = mk(synth_coff_out(ts), synth_map("app.out", ctime_utc(ts)))      # 이후 시험용으로 정상 패키지 복원
        good = R("t.dsppkg")

        # 7. verify: 변조·구조 오류
        def entries(pkg):
            with zipfile.ZipFile(io.BytesIO(pkg)) as z:
                return [(i.filename, z.read(i)) for i in z.infolist()]

        def zipped(ents):
            buf = io.BytesIO()
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")                    # 중복 이름 경고
                with zipfile.ZipFile(buf, "w") as z:               # 저장(stored) 방식도 허용
                    for n, b in ents:
                        z.writestr(zipfile.ZipInfo(n, (2026, 10, 31, 0, 0, 0)), b)
            return buf.getvalue()

        def edit_mf(pkg, fn):
            out = []
            for n, b in entries(pkg):
                if n == PKG_MANIFEST:
                    m = json.loads(b)
                    fn(m)
                    b = _json_bytes(m)
                out.append((n, b))
            return zipped(out)

        def edit_ent(pkg, name, fn):
            return zipped([(n, fn(b) if n == name else b) for n, b in entries(pkg)])

        ents = entries(good)
        assert verify_package(zipped(ents))["pkg_id"] == r["pkg_id"], "stored 방식도 읽힘"
        up = edit_mf(good, lambda m: m["files"]["out"].__setitem__("sha256", m["files"]["out"]["sha256"].upper()))
        assert verify_package(up)["link_check"] == "time", "sha256 은 대소문자 무시"
        assert verify_package(edit_mf(good, lambda m: m.__setitem__("future", {"x": 1})))["ver"] == "1.2.3", "알 수 없는 키는 무시"
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["out"].__setitem__("sha256", "0" * 64))), "sha256")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["map"].__setitem__("size", 1))), "size")
        fails(lambda: verify_package(edit_ent(good, "app.out", lambda b: b[:-1] + bytes([b[-1] ^ 1]))), "sha256")       # 내용 변조, 크기 같음
        fails(lambda: verify_package(edit_ent(good, "app_1.2.3.dspfw", lambda b: b[:-1] + bytes([b[-1] ^ 1]))), "sha256")
        fails(lambda: verify_package(zipped(ents + [("readme.txt", b"x")])), "허용되지 않는 항목")
        fails(lambda: verify_package(zipped(ents + [("extra.out", b"x")])), "둘 이상")
        fails(lambda: verify_package(zipped(ents + [("sub/", b"")])), "디렉터리")
        fails(lambda: verify_package(zipped(ents + [ents[3]])), "중복")
        for bad in ("../x.out", "sub/x.out", "a\\x.out", "x..y.out", "with space.out", "한글.out", "." + "a.out", "a" * 57 + ".out"):
            fails(lambda: verify_package(zipped([(bad, b) if n == "app.out" else (n, b) for n, b in ents])), "규칙에 어긋남")
        fails(lambda: verify_package(zipped([e for e in ents if e[0] != "app.map"])), "빠진 항목")
        fails(lambda: verify_package(zipped([e for e in ents if e[0] != PKG_MANIFEST])), "빠진 항목")
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("ver", "1.2.4"))), "1.2.4")                 # manifest ↔ .dspfw 버전
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("ver", "1.2.3-rc1"))), "버전 문자열")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["dspfw"].__setitem__("app_crc32", "00000000"))), "app_crc32")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["dspfw"].__setitem__("app_len", 1))), "app_len")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["dspfw"].__setitem__("build_time", 1))), "build_time")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["out"].__setitem__("timestamp", ts + 1))), "files.out.timestamp")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["map"].__setitem__("output", "x.out"))), "files.map.output")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["dspfw"].__setitem__("name", "z.dspfw"))), "name")
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("format", "x"))), "패키지 형식이 아님")
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("format_ver", 2))), "패키지 형식이 아님")
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("format_ver", True))), "패키지 형식이 아님")
        fails(lambda: verify_package(edit_mf(good, lambda m: m.pop("files"))), "files")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["out"].__setitem__("size", "1"))), "size")
        fails(lambda: verify_package(edit_mf(good, lambda m: m["files"]["out"].__setitem__("orig_name", ""))), "orig_name")
        fails(lambda: verify_package(edit_ent(good, PKG_MANIFEST, lambda b: b"not json")), "manifest.json 을 읽지 못함")
        fails(lambda: verify_package(edit_ent(good, PKG_MANIFEST, lambda b: b"[1]")), "JSON 객체")
        # 링크 확인은 읽는 쪽이 다시 돌린다: 틀린 link_check 주장은 거부, skipped 는 경고와 함께 받음
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("link_check", "name"))), "다시 계산한 값")
        fails(lambda: verify_package(edit_mf(good, lambda m: m.__setitem__("link_check", "bogus"))), "link_check")
        bad_map = zipped([(n, synth_map("app.out", ctime_utc(ts + 1000))) if n == "app.map" else (n, b) for n, b in ents])
        bad_map = edit_mf(bad_map, lambda m: m["files"]["map"].update(sha256=hashlib.sha256(synth_map("app.out", ctime_utc(ts + 1000))).hexdigest(),
                                                                       size=len(synth_map("app.out", ctime_utc(ts + 1000))),
                                                                       linked=ctime_utc(ts + 1000)))
        fails(lambda: verify_package(bad_map), "링크 시각이 다름")
        sk = verify_package(edit_mf(bad_map, lambda m: m.__setitem__("link_check", "skipped")))
        assert sk["link_check"] == "skipped" and any("skipped" in w for w in sk["warnings"])
        # 크기 한도 (시험용 덮어쓰기), 잘린·깨진·다른 형식 파일
        for kind in ("out", "map", "dspfw", "manifest"):
            fails(lambda: verify_package(good, limits={kind: 10}), "크기 한도")
        assert verify_package(good, limits={"out": len(synth_coff_out(ts))})["ver"] == "1.2.3", "한도와 같은 크기는 통과"
        assert PKG_LIMITS["dspfw"] == 393152 and PKG_LIMITS["out"] == 64 << 20 and PKG_LIMITS["map"] == 16 << 20 and PKG_LIMITS["manifest"] == 65536
        fails(lambda: verify_package(good[:len(good) // 2]), "ZIP")
        fails(lambda: verify_package(b"hello"), "ZIP")
        fails(lambda: verify_package(want), "ZIP")                    # 맨 .dspfw 는 패키지가 아님
    print("package ok (bundle with fake hex2000, time/name link rule, --no-link-check, verify tamper/structure/limit checks)")


def cmd_selftest(a):
    # 표준 검사값: CRC32("123456789") = CBF43926, CRC-16/CCITT-FALSE("123456789") = 29B1
    assert crc32_final(crc32_bytes(b"123456789")) == 0xCBF43926
    assert crc16_bytes(b"123456789") == 0x29B1
    words = [0x3231, 0x3433, 0x3635, 0x3837]          # "12345678" 을 하위 바이트 먼저 워드로
    print(f"crc32_words(12345678)  = {crc32_words(words):08X}")
    print(f"crc16_words(12345678)  = {crc16_words(words):04X}")
    hdr = build_header(0x1234, 0xDEADBEEF, 1700000000, "1.2.3")
    print(f"hdr[62] (hdr_crc16)    = {hdr[62]:04X}")
    # Intel HEX 파서: 워드 주소(--memwidth=16 --romwidth=16) 는 그대로, 바이트 주소는 거부
    import tempfile, os
    def rec(addr, data):
        b = bytes([len(data), addr >> 8, addr & 0xFF, 0]) + data
        return ":" + (b + bytes([(-sum(b)) & 0xFF])).hex().upper()
    ext = lambda hi: ":02000004" + f"{hi:04X}" + f"{(-(2 + 4 + (hi >> 8) + (hi & 0xFF))) & 0xFF:02X}"
    with tempfile.TemporaryDirectory() as d:
        wp = os.path.join(d, "w.hex")
        with open(wp, "w") as f:
            f.write(ext(0x003E) + "\n" + rec(0x8040, bytes([0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44])) + "\n:00000001FF\n")
        m = parse_intel_hex(wp)
        assert m == {0x3E8040: 0x1111, 0x3E8041: 0x2222, 0x3E8042: 0x3333, 0x3E8043: 0x4444}, m
        bp = os.path.join(d, "b.hex")
        with open(bp, "w") as f:
            f.write(ext(0x007D) + "\n" + rec(0x0080, bytes([0x11, 0x11, 0x22, 0x22])) + "\n:00000001FF\n")
        try:
            parse_intel_hex(bp)
            raise AssertionError("바이트 주소 HEX 를 거부해야 함")
        except ValueError as e:
            assert "바이트 주소" in str(e), e
    print("intel hex parse ok (word addresses accepted, byte addresses rejected)")
    # 버전 문자열 규칙 (8자 고정 R 응답 칸에 들어가야 서버 비교가 맞음)
    for good in ("1.2.3", "12345678", "v1_0-rc", "A"):
        check_ver(good)
        build_header(1, 0, 0, good)
    for bad in ("", "1.2.3-rc1", "1.2 3", " 1.2", "1.2.3\t", "버전", "1/2", "1.2.3\n"):
        try:
            build_header(1, 0, 0, bad)
            raise AssertionError(f"버전 {bad!r} 을 거부해야 함")
        except ValueError as e:
            assert "버전 문자열" in str(e), e
    mem1 = {APP_CODE_ADDR: 0x1234}
    try:
        pack(mem1, "1.2.3-rc1", 0)
        raise AssertionError("pack 이 9자 버전을 거부해야 함")
    except ValueError:
        pass
    img = pack(mem1, "1.2.3", 1700000000)
    info = unpack(img)
    assert info["ver"] == "1.2.3" and info["app_len"] == 1 and img[16:24] == b"1.2.3\0\0\0", info
    print("version rule ok (1~8 chars of [0-9A-Za-z._-])")
    # 블록 순서: 같은 데이터라도 내림차순·겹친 블록은 거부 (브리지·게이트웨이와 같은 규칙)
    img2 = pack({APP_CODE_ADDR: 0x1111, APP_CODE_ADDR + BLOCK_WORDS: 0x2222}, "1.2.3", 0)
    blk = unpack(img2)["blocks"]
    assert [a for a, _ in blk] == [APP_CODE_ADDR, APP_CODE_ADDR + BLOCK_WORDS], blk
    def rebuild(order):
        body = img2[FILE_HDR_LEN:FILE_HDR_LEN + HDR_WORDS * 2] + b"".join(struct.pack("<IH", a, len(ws)) + words_to_le(ws) for a, ws in order)
        return FILE_MAGIC + struct.pack("<II", FILE_HDR_LEN + len(body), crc32_final(crc32_bytes(body))) + img2[16:FILE_HDR_LEN] + body
    assert unpack(rebuild(blk))["blocks"] == blk
    for order in (blk[::-1], [blk[0], (APP_CODE_ADDR + BLOCK_WORDS - 1, blk[1][1])]):
        try:
            unpack(rebuild(order))
            raise AssertionError(f"블록 순서·겹침을 거부해야 함: {[hex(a) for a, _ in order]}")
        except ValueError as e:
            assert "순서" in str(e), e
    print("block order ok (ascending, no overlap)")
    selftest_package()
    print("selftest ok")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    q = sp.add_parser("pack", help="Intel HEX → .dspfw")
    q.add_argument("hexfile")
    q.add_argument("-o", "--output", required=True)
    q.add_argument("--ver", required=True, help=f"앱 버전 문자열: {VER_RULE}. R 응답 버전 칸이 8자라 더 길면 서버 비교 실패")
    q.add_argument("--build", help="빌드 시각 unix 초 (기본: 지금)")
    q.set_defaults(fn=cmd_pack)
    q = sp.add_parser("bundle", help=".out + .map → .dsppkg (hex2000 으로 .out 에서 .dspfw 를 만들어 함께 묶음)",
                      description="메인보드 프로그램 패키지 만들기. .out/.map 은 필수 — 원본 소스를 잃어도 .out(DWARF)·.map 으로 복구할 수 있게 함께 보관한다.")
    q.add_argument("outfile", metavar="app.out", help="CCS 링커 출력 (.out, TI COFF 또는 ELF)")
    q.add_argument("--map", required=True, help="같은 빌드의 링커 맵 파일 (.map)")
    q.add_argument("--ver", required=True, help=f"앱 버전 문자열: {VER_RULE}")
    q.add_argument("-o", "--output", help="출력 .dsppkg (기본: .out 옆의 <이름>_<ver>.dsppkg)")
    q.add_argument("--hex2000", help="hex2000 경로 (기본: 환경변수 HEX2000 → PATH → Windows C:\\ti\\ccs*\\... 중 가장 새 것). "
                                     ".py 파일이면 sys.executable 로 실행 — 시험용 가짜 hex2000 전용")
    q.add_argument("--build", help="빌드 시각 unix 초 (기본: 지금)")
    q.add_argument("--no-link-check", action="store_true",
                   help=".map 과 .out 의 연결 확인(링크 시각·이름)을 건너뜀 — 실기 .out/.map 으로 검증 전이라 막힐 때만. link_check=skipped 로 기록되고 경고가 나옴")
    q.set_defaults(fn=cmd_bundle)
    q = sp.add_parser("verify", help=".dsppkg 전체 검증 (통과 0, 실패 1)")
    q.add_argument("file")
    q.set_defaults(fn=cmd_verify)
    q = sp.add_parser("info", help=".dsppkg 또는 .dspfw 내용 표시·검증")
    q.add_argument("file")
    q.set_defaults(fn=cmd_info)
    q = sp.add_parser("selftest", help="CRC 테스트 벡터·HEX 파서·버전 규칙·패키지 검사")
    q.set_defaults(fn=cmd_selftest)
    a = p.parse_args()
    for s in (sys.stdout, sys.stderr):      # CCS post-build 처럼 출력이 파이프일 때 로케일(cp949)에 없는 글자 때문에 죽지 않게
        try:
            s.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    try:
        a.fn(a)
    except (ValueError, OSError, zipfile.BadZipFile) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
