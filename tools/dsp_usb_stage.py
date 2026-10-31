#!/usr/bin/env python3
"""메인보드 프로그램(.dsppkg)을 USB 로 브리지의 dspfw 파티션에 직접 넣기 — 검수용 (협력사가 만든 신규 AGV)

  dsp_usb_stage.py COM5 AGV_Main_1.2.3.dsppkg                          # 파티션 표를 보드에서 읽어 dspfw 파티션만 씀 (설정 유지)
  dsp_usb_stage.py COM5 AGV_Main_1.2.3.dsppkg --bridge agv_wifi_bridge_merged.bin
                                                                       # 브리지 전체 이미지 + 스테이징을 한 번에 (저장된 호기 설정이 지워짐)
  dsp_usb_stage.py - AGV_Main_1.2.3.dsppkg --write-blob stage.bin      # 보드 없이 스테이징 이미지만 파일로 (시험용)

  옵션: --baud 921600  --esptool "python -m esptool"

패키지(.dsppkg)만 받는다 — 맨 .dspfw 는 거부한다 (.out/.map 보관이 목적). 패키지는 dsp_pack.py verify_package 로 전부 검증한 뒤 쓴다.
쓰고 나면 브리지가 재부팅하며 스테이징을 인식한다. 이어서 브리지 콘솔/BLE 에서 `dsp` 로 확인하고 AGV 를 세운 상태에서 `dsp apply`.
규격: docs/DSP_OTA_DESIGN.md 3-1절(패키지), 스테이징 머리는 main/dsp_ota.c 의 stage_hdr_t 와 같음. esptool 필요 (pip install esptool).
"""
import argparse
import glob
import importlib.util
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsp_pack  # noqa: E402

STAGE_MAGIC     = b"DSPSTG1\0"                  # main/dsp_ota.c STAGE_MAGIC (8바이트, NUL 포함)
STAGE_HDR_LEN   = dsp_pack.STAGE_HDR_LEN        # 64
PT_OFFSET       = 0x8000                        # 파티션 표 위치 (ESP-IDF 기본)
PT_SIZE         = 0xC00
PT_ENTRY        = 32
PT_MAGIC        = b"\xAA\x50"
PT_MD5_MAGIC    = b"\xEB\xEB"
PT_TYPE_DATA    = 0x01
DSPFW_LABEL     = "dspfw"
DSPFW_SUBTYPE   = 0x40
# partitions.csv 의 dspfw 기본값 — 실제 위치는 늘 보드/이미지의 파티션 표에서 읽고, 다르면 경고만 한다
DSPFW_DEFAULT_OFFSET = 0x3A0000
DSPFW_DEFAULT_SIZE   = dsp_pack.STAGE_PART_SIZE   # 0x60000
SECTOR          = 0x1000


# ───────── 스테이징 이미지 ─────────

def build_stage_blob(dspfw: bytes, ver: str, app_crc: int, pkg_id: str) -> bytes:
    """브리지 dspfw 파티션 오프셋 0 에 들어가는 이미지: 머리 64 B (stage_hdr_t) + .dspfw 원본.
    job=0(작업 번호 없음 — 콘솔·BLE `dsp apply` 로 적용), rsvd[0..15] = 패키지 ID ASCII 16자, rsvd[16..23] = 0 (브리지는 rsvd 를 읽지 않음)"""
    if len(dspfw) < dsp_pack.FILE_HDR_LEN or dspfw[:8] != dsp_pack.FILE_MAGIC:
        raise ValueError(".dspfw 형식이 아님")
    file_crc = struct.unpack_from("<I", dspfw, 12)[0]           # .dspfw 파일 머리의 CRC
    if not re.fullmatch(r"[0-9a-f]{16}", pkg_id):
        raise ValueError(f"패키지 ID 형식 오류: {pkg_id!r}")
    hdr = struct.pack("<8sIIII16s24s", STAGE_MAGIC, 0, len(dspfw), file_crc, app_crc,
                      ver.encode("ascii"), pkg_id.encode("ascii") + bytes(8))
    assert len(hdr) == STAGE_HDR_LEN
    return hdr + dspfw


# ───────── 파티션 표 ─────────

def parse_partition_table(data: bytes) -> list:
    """ESP-IDF 파티션 표 (32바이트 항목: magic AA 50, type, subtype, offset, size, label[16], flags).
    0xFF 로 찬 항목이나 MD5 항목(EB EB)에서 끝난다. [{label, type, subtype, offset, size, flags}, ...]"""
    out = []
    for i in range(0, len(data) - PT_ENTRY + 1, PT_ENTRY):
        rec = data[i:i + PT_ENTRY]
        if rec[:2] == PT_MD5_MAGIC or rec == b"\xFF" * PT_ENTRY:
            break
        if rec[:2] != PT_MAGIC:
            raise ValueError(f"파티션 표가 아님 (항목 {i // PT_ENTRY} 의 magic {rec[:2].hex()}) — ESP32-S3 브리지 보드/이미지가 맞는지 확인")
        typ, sub, off, size = struct.unpack_from("<BBII", rec, 2)
        out.append({"label": rec[12:28].split(b"\0", 1)[0].decode("ascii", "replace"), "type": typ, "subtype": sub,
                    "offset": off, "size": size, "flags": struct.unpack_from("<I", rec, 28)[0]})
    if not out:
        raise ValueError("파티션 표가 비어 있음 (브리지 펌웨어가 구워져 있지 않은 보드/이미지)")
    return out


def find_dspfw_partition(table: list):
    """dspfw 파티션 항목 (없으면 None). 형식(data/0x40)·4KB 정렬이 다르면 ValueError"""
    for e in table:
        if e["label"] == DSPFW_LABEL:
            if e["type"] != PT_TYPE_DATA or e["subtype"] != DSPFW_SUBTYPE:
                raise ValueError(f"dspfw 파티션 형식이 예상과 다름 (type {e['type']:#x}, subtype {e['subtype']:#x}; data/0x40 이어야 함)")
            if e["offset"] % SECTOR:
                raise ValueError(f"dspfw 파티션 오프셋 0x{e['offset']:X} 가 4KB 경계가 아님")
            return e
    return None


NO_DSPFW_MSG = ("dspfw 파티션이 없음 — 이 브리지는 0.5.0 이전의 파티션 배치입니다. 전체 이미지를 함께 굽는 "
                "--bridge agv_wifi_bridge_merged.bin 을 주세요 (이 경우 저장된 호기 설정이 지워집니다)")


# ───────── esptool ─────────

def _split_cmd(s: str) -> list:
    if os.name == "nt":                     # 백슬래시 경로가 깨지지 않게
        parts = shlex.split(s, posix=False)
        return [p[1:-1] if len(p) >= 2 and p[0] == p[-1] and p[0] in "\"'" else p for p in parts]
    return shlex.split(s)


def find_esptool(explicit=None) -> list:
    """esptool 실행 명령. 순서: --esptool → import 가능한 esptool 모듈 → Arduino IDE ESP32 코어 내장 esptool.exe → PATH 의 esptool/esptool.py"""
    if explicit:
        parts = _split_cmd(explicit)
        if not parts:
            raise ValueError("--esptool 이 비어 있음")
        return parts
    if importlib.util.find_spec("esptool") is not None:
        return [sys.executable, "-m", "esptool"]
    la = os.environ.get("LOCALAPPDATA")
    if la:
        found = glob.glob(os.path.join(la, "Arduino15", "packages", "esp32", "tools", "esptool_py", "*", "esptool.exe"))
        if found:
            return [max(found, key=lambda f: ([int(x) for x in re.findall(r"\d+", f)], f))]
    for name in ("esptool", "esptool.py"):
        p = shutil.which(name)
        if p:
            return [p]
    raise ValueError("esptool 을 찾지 못함 — pip install esptool (또는 Arduino IDE 에 ESP32 보드 설치). --esptool 로 직접 지정할 수도 있음")


def run_esptool(esptool: list, port: str, baud: int, after: str, args: list):
    cmd = esptool + ["--chip", "esp32s3", "-p", port, "-b", str(baud), "--before", "default_reset", "--after", after] + args
    print("esptool: " + " ".join(shlex.quote(c) for c in cmd[len(esptool):]), file=sys.stderr)
    try:
        rc = subprocess.call(cmd)
    except OSError as e:
        raise ValueError(f"esptool 을 실행하지 못함 ({' '.join(esptool)}): {e}")
    if rc != 0:
        raise ValueError(f"esptool 실패 (종료 코드 {rc}). 포트가 사용 중이면 시리얼 모니터를 닫고, 자동 리셋이 안 되는 보드는 "
                         "BOOT 를 누른 채 RESET 을 눌렀다 놓은 뒤 다시 실행하세요")


def read_board_table(esptool: list, port: str, baud: int) -> bytes:
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "ptable.bin")
        run_esptool(esptool, port, baud, "no_reset", ["read_flash", hex(PT_OFFSET), hex(PT_SIZE), p])
        with open(p, "rb") as f:
            data = f.read()
    if len(data) < PT_ENTRY:
        raise ValueError("보드에서 파티션 표를 읽지 못함")
    return data


# ───────── 실행 ─────────

def load_package(path: str) -> dict:
    """.dsppkg 를 전부 검증해서 읽는다. 패키지가 아니면(맨 .dspfw 포함) 거부"""
    with open(path, "rb") as f:
        head = f.read(8)
    if head == dsp_pack.FILE_MAGIC:
        raise ValueError("맨 .dspfw 는 USB 로 넣을 수 없음 — .out/.map 을 함께 보관한 패키지(.dsppkg)만 받습니다. "
                         "dsp_pack.py bundle app.out --map app.map --ver <버전> 으로 패키지를 만드세요")
    if head[:4] not in (b"PK\x03\x04", b"PK\x05\x06"):
        raise ValueError("패키지(.dsppkg)가 아님 — dsp_pack.py bundle 로 만든 파일이어야 합니다")
    return dsp_pack.verify_package(path)


def check_fit(blob: bytes, part: dict):
    if len(blob) > part["size"]:
        raise ValueError(f"스테이징 이미지({len(blob)} B)가 dspfw 파티션(0x{part['size']:X} = {part['size']} B)보다 큼")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="브리지 USB 시리얼 포트 (예: COM5, /dev/ttyUSB0). --write-blob 일 때는 '-' 가능")
    p.add_argument("package", help="메인보드 프로그램 패키지 (.dsppkg)")
    p.add_argument("--bridge", metavar="MERGED.bin", help="브리지 전체 이미지(agv_wifi_bridge_merged.bin)도 함께 굽기. "
                   "보드의 파티션 표에 dspfw 가 없을 때(0.5.0 이전)에 쓰며, 저장된 호기 설정이 지워짐")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--esptool", metavar="CMD", help='esptool 실행 명령 (기본: 자동 탐색). 예: "python -m esptool"')
    p.add_argument("--write-blob", metavar="FILE", help="스테이징 이미지(머리 64 B + .dspfw)를 파일로만 쓰고 끝냄 — 보드를 건드리지 않음 (시험용)")
    a = p.parse_args()
    for s in (sys.stdout, sys.stderr):      # 출력이 파이프일 때 로케일에 없는 글자 때문에 죽지 않게
        try:
            s.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    try:
        res = load_package(a.package)
        for w in res["warnings"]:
            print(f"warning: {w}", file=sys.stderr)
        di = res["dspfw_info"]
        blob = build_stage_blob(res["dspfw_bytes"], res["ver"], di["app_crc"], res["pkg_id"])
        print(f"package: {os.path.basename(a.package)}  ver {res['ver']}, pkg id {res['pkg_id']}, link_check {res['link_check']}, "
              f".dspfw {len(res['dspfw_bytes'])} B -> 스테이징 이미지 {len(blob)} B", file=sys.stderr)

        merged = None
        if a.bridge:
            with open(a.bridge, "rb") as f:
                merged = f.read()
            if len(merged) < PT_OFFSET + PT_ENTRY or merged[0] != 0xE9:
                raise ValueError(f"브리지 전체 이미지가 아님: {a.bridge} (agv_wifi_bridge_merged.bin 이어야 함)")
            part = find_dspfw_partition(parse_partition_table(merged[PT_OFFSET:PT_OFFSET + PT_SIZE]))
            if part is None:
                raise ValueError(f"{a.bridge} 의 파티션 표에 dspfw 가 없음 — 0.5.0 이상 브리지의 전체 이미지가 필요합니다")
            if len(merged) > part["offset"]:
                raise ValueError(f"전체 이미지({len(merged)} B)가 dspfw 파티션 시작(0x{part['offset']:X})을 넘어 겹침")

        if a.write_blob:
            part = part if merged else {"offset": DSPFW_DEFAULT_OFFSET, "size": DSPFW_DEFAULT_SIZE}
            check_fit(blob, part)
            with open(a.write_blob, "wb") as f:
                f.write(blob)
            print(f"wrote {a.write_blob} ({len(blob)} B) — 보드는 건드리지 않음", file=sys.stderr)
            return
        if a.port == "-":
            raise ValueError("포트를 지정하세요 (예: COM5). '-' 는 --write-blob 과 함께만 씁니다")

        esptool = find_esptool(a.esptool)
        if not merged:
            print(f"보드의 파티션 표를 읽습니다 (0x{PT_OFFSET:X}, {PT_SIZE} B)...", file=sys.stderr)
            part = find_dspfw_partition(parse_partition_table(read_board_table(esptool, a.port, a.baud)))
            if part is None:
                raise ValueError(NO_DSPFW_MSG)
        if (part["offset"], part["size"]) != (DSPFW_DEFAULT_OFFSET, DSPFW_DEFAULT_SIZE):
            print(f"warning: dspfw 파티션이 기본값(0x{DSPFW_DEFAULT_OFFSET:X}, 0x{DSPFW_DEFAULT_SIZE:X})과 다름: "
                  f"0x{part['offset']:X}, 0x{part['size']:X} — 표의 값을 따릅니다", file=sys.stderr)
        check_fit(blob, part)

        with tempfile.TemporaryDirectory() as d:
            bp = os.path.join(d, "dspfw_stage.bin")
            with open(bp, "wb") as f:
                f.write(blob)
            if merged:
                print("주의: --bridge 로 브리지 전체 이미지를 굽습니다 — 저장된 호기 설정(NVS)이 지워지니 끝난 뒤 다시 설정해야 합니다.", file=sys.stderr)
                run_esptool(esptool, a.port, a.baud, "hard_reset", ["write_flash", "0x0", a.bridge, hex(part["offset"]), bp])
            else:
                print("호기 설정은 그대로 두고 dspfw 파티션만 씁니다.", file=sys.stderr)
                run_esptool(esptool, a.port, a.baud, "hard_reset", ["write_flash", hex(part["offset"]), bp])
        print(f"""
완료. 브리지가 재부팅되며 스테이징을 인식합니다. 다음 순서:
  1. 브리지 콘솔(115200) 또는 BLE 터미널에서 `dsp` 를 입력해 staged={res['ver']}(job 0) 으로 보이는지 확인
  2. AGV 를 세운 상태에서 `dsp apply` (앱에 진입 요청 -> 부트로더 -> 지우기·쓰기·검증·리셋, 약 20~30초)
  3. 다시 `dsp` 로 app={res['ver']} 인지 메인보드 버전 확인
※ 메인보드에 상주 부트로더(1.1.0)가 JTAG 으로 먼저 구워져 있어야 합니다 (docs/FLASH_GUIDE.md 8장).""")
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
