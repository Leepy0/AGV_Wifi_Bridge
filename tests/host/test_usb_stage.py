#!/usr/bin/env python3
"""패키지(.dsppkg) 만들기·USB 스테이징·꺼내기 도구 시험 — TI 도구·보드 없이 (가짜 hex2000, 가짜 esptool)

  - 기준 HEX(golden/app_golden.hex) → 가짜 hex2000 → dsp_pack.py bundle → info / verify → dsp_usb_stage.py --write-blob
  - 스테이징 머리·보관 슬롯 머리를 main/dsp_ota.c 의 stage_hdr_t·main/dsp_pkg.c 의 pkg_hdr_t 배치와 바이트 단위로 대조,
    partitions.csv·main/dsp_pkg.h 와 기본값 대조
  - 파티션 표 해석기, 슬롯 선택 규칙, esptool 탐색, 가짜 esptool 로 호출 순서·인자
    (보드에서 표·슬롯 머리 읽기 → write_flash 한 번 / --bridge 한 번에 굽기 / dsp_pkg_read.py 로 꺼내기)
tests/host/run.sh 가 실행한다.
"""
import hashlib
import json
import os
import re
import shlex
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

HT = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HT, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dsp_pack as dp    # noqa: E402
import dsp_usb_stage as us    # noqa: E402
import dsp_pkg_read as pr    # noqa: E402

PACK = os.path.join(ROOT, "tools", "dsp_pack.py")
STAGE = os.path.join(ROOT, "tools", "dsp_usb_stage.py")
READ = os.path.join(ROOT, "tools", "dsp_pkg_read.py")
TS = 1793422810


def read(p, mode="rb"):
    with open(p, mode, **({} if "b" in mode else {"encoding": "utf-8"})) as f:
        return f.read()


def write(p, data):
    with open(p, "wb") as f:
        f.write(data)
    return p


def run(args, env=None, ok=True):
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run([sys.executable] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=e)
    out, err = r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")
    assert (r.returncode == 0) == ok, f"{args[:3]} 종료 코드 {r.returncode}\n{out}\n{err}"
    return out, err


# ───────── main/dsp_ota.c 의 stage_hdr_t 배치 ─────────

def c_layout(path, name):
    src = read(os.path.join(ROOT, "main", path), "r")
    m = re.search(r"typedef struct \{(.*?)\} " + name + ";", src, re.S)
    assert m, f"{name} 를 찾지 못함"
    size = {"char": 1, "uint8_t": 1, "uint32_t": 4}
    off, fields = 0, {}
    for line in m.group(1).splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        t, fname, n = re.fullmatch(r"(\w+)\s+(\w+)(?:\[(\d+)\])?;", line).groups()
        off = (off + size[t] - 1) // size[t] * size[t]            # 자연 정렬
        fields[fname] = (off, size[t] * (int(n) if n else 1))
        off += fields[fname][1]
    return fields, off, src


def c_define(src, k):
    return re.search(rf'#define\s+{k}\s+(\S+)', src).group(1)


def c_stage_layout():
    src = read(os.path.join(ROOT, "main", "dsp_ota.c"), "r")
    m = re.search(r"typedef struct \{(.*?)\} stage_hdr_t;", src, re.S)
    assert m, "stage_hdr_t 를 찾지 못함"
    size = {"char": 1, "uint8_t": 1, "uint32_t": 4}
    off, fields = 0, {}
    for line in m.group(1).splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        t, name, n = re.fullmatch(r"(\w+)\s+(\w+)(?:\[(\d+)\])?;", line).groups()
        off = (off + size[t] - 1) // size[t] * size[t]            # 자연 정렬
        fields[name] = (off, size[t] * (int(n) if n else 1))
        off += fields[name][1]
    consts = {k: re.search(rf'#define\s+{k}\s+(\S+)', src).group(1) for k in ("STAGE_HDR_LEN", "STAGE_MAGIC", "PART_LABEL")}
    return fields, off, consts


def csv_rows():
    rows = []
    for ln in read(os.path.join(ROOT, "partitions.csv"), "r").splitlines():
        if ln.strip() and not ln.lstrip().startswith("#"):
            rows.append([c.strip() for c in ln.split(",")])
    return rows


SUBTYPE = {"nvs": 0x02, "ota": 0x00, "phy": 0x01, "ota_0": 0x10, "ota_1": 0x11}


def table_bin(rows, md5=True) -> bytes:
    """partitions.csv 행 → ESP-IDF 파티션 표 바이너리 (0x8000 의 0xC00 B, 끝에 MD5 항목, 나머지 0xFF)"""
    b = b""
    for name, typ, sub, off, size, *_ in rows:
        t = {"app": 0, "data": 1}[typ]
        s = SUBTYPE[sub] if sub in SUBTYPE else int(sub, 0)
        b += b"\xAA\x50" + struct.pack("<BBII", t, s, int(off, 0), int(size, 0)) + name.encode().ljust(16, b"\0") + bytes(4)
    if md5:
        b += b"\xEB\xEB" + b"\xFF" * 14 + hashlib.md5(b).digest()
    return b.ljust(us.PT_SIZE, b"\xFF")


def main():
    # ── 1. 상수·배치가 브리지 소스·partitions.csv 와 같다 ──
    fields, total, consts = c_stage_layout()
    assert list(fields) == ["magic", "job", "file_len", "file_crc", "app_crc", "ver", "rsvd"], list(fields)
    assert fields == {"magic": (0, 8), "job": (8, 4), "file_len": (12, 4), "file_crc": (16, 4), "app_crc": (20, 4),
                      "ver": (24, 16), "rsvd": (40, 24)}, fields
    assert total == 64 == int(consts["STAGE_HDR_LEN"]) == us.STAGE_HDR_LEN == dp.STAGE_HDR_LEN
    assert consts["STAGE_MAGIC"].strip('"').encode() + b"\0" == us.STAGE_MAGIC == b"DSPSTG1\0"
    assert consts["PART_LABEL"].strip('"') == us.DSPFW_LABEL == "dspfw"
    rows = csv_rows()
    row = next(r for r in rows if r[0] == "dspfw")
    assert (row[1], int(row[2], 0), int(row[3], 0), int(row[4], 0)) == ("data", us.DSPFW_SUBTYPE, us.DSPFW_DEFAULT_OFFSET, us.DSPFW_DEFAULT_SIZE), row
    assert int(row[4], 0) == dp.STAGE_PART_SIZE and dp.DSPFW_MAX == dp.STAGE_PART_SIZE - 64 == 393152
    # 보관 슬롯: dsp_pkg.c pkg_hdr_t·dsp_pkg.h 상수·partitions.csv dsppkg (16MB 끝까지)
    pf, ptotal, psrc = c_layout("dsp_pkg.c", "pkg_hdr_t")
    assert pf == {"magic": (0, 8), "flags": (8, 4), "seq": (12, 4), "pkg_size": (16, 4), "job": (20, 4), "dspfw_crc": (24, 4),
                  "pkg_id": (28, 17), "ver": (45, 16), "sha256": (61, 32), "rsvd": (93, 35)}, pf
    assert ptotal == 128 == int(c_define(psrc, "HDR_LEN")) == struct.calcsize(us.PKG_HDR_FMT)
    assert c_define(psrc, "HDR_MAGIC").strip('"').encode() + b"\0" == us.PKG_MAGIC == b"DSPPKG1\0"
    hsrc = read(os.path.join(ROOT, "main", "dsp_pkg.h"), "r")
    assert c_define(hsrc, "DSP_PKG_LABEL").strip('"') == us.DSPPKG_LABEL == "dsppkg"
    assert int(c_define(hsrc, "DSP_PKG_SLOT_SIZE").rstrip("u"), 0) == us.PKG_SLOT_SIZE == 0x600000
    assert int(c_define(hsrc, "DSP_PKG_HDR_SECTOR").rstrip("u"), 0) == us.PKG_HDR_SECTOR == 0x1000
    assert int(c_define(hsrc, "DSP_PKG_SLOTS")) == us.PKG_SLOTS == 2 and us.PKG_MAX == 6287360 == dp.BRIDGE_PKG_MAX
    prow = next(r for r in rows if r[0] == "dsppkg")
    assert (prow[1], int(prow[2], 0), int(prow[3], 0), int(prow[4], 0)) == ("data", us.DSPPKG_SUBTYPE, us.DSPPKG_DEFAULT_OFFSET,
                                                                           us.DSPPKG_DEFAULT_SIZE) == ("data", 0x41, 0x400000, 0xC00000), prow
    assert us.DSPPKG_DEFAULT_OFFSET + us.DSPPKG_DEFAULT_SIZE == 16 << 20
    # 기존 항목은 0.8.x 와 같은 위치 (표 + 앱만 다시 구우면 nvs·스테이징 유지)
    assert [(r[0], r[3], r[4]) for r in rows if r[0] != "dsppkg"] == [
        ("nvs", "0x9000", "0x6000"), ("otadata", "0xf000", "0x2000"), ("phy_init", "0x11000", "0x1000"),
        ("ota_0", "0x20000", "0x1C0000"), ("ota_1", "0x1E0000", "0x1C0000"), ("dspfw", "0x3A0000", "0x60000")], rows
    ends = sorted((int(r[3], 0), int(r[3], 0) + int(r[4], 0)) for r in rows)
    assert all(a[1] <= b[0] for a, b in zip(ends, ends[1:])), ends
    assert "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y" in read(os.path.join(ROOT, "sdkconfig.defaults"), "r")
    print("constants ok (stage_hdr_t / pkg_hdr_t layout, magics, labels, partitions.csv dspfw + dsppkg, 16MB)")

    # ── 2. 파티션 표 해석기 ──
    table = us.parse_partition_table(table_bin(rows))
    assert [(e["label"], e["offset"], e["size"]) for e in table] == [(r[0], int(r[3], 0), int(r[4], 0)) for r in rows]
    part = us.find_dspfw_partition(table)
    assert (part["offset"], part["size"], part["type"], part["subtype"]) == (0x3A0000, 0x60000, 1, 0x40), part
    assert us.parse_partition_table(table_bin(rows, md5=False)) == table                  # MD5 항목 없이 0xFF 로 끝나도 됨
    pp = us.find_dsppkg_partition(table)
    assert (pp["offset"], pp["size"], pp["subtype"]) == (0x400000, 0xC00000, 0x41), pp
    old = [r for r in rows if r[0] not in ("dspfw", "dsppkg")]
    assert us.find_dspfw_partition(us.parse_partition_table(table_bin(old))) is None        # 0.5.0 이전 배치
    old8 = [r for r in rows if r[0] != "dsppkg"]                                            # 0.5.0~0.8.x 배치 (4MB)
    assert us.find_dsppkg_partition(us.parse_partition_table(table_bin(old8))) is None
    try:
        us.find_dsppkg_partition(us.parse_partition_table(table_bin([(r[:4] + ["0x600000"]) if r[0] == "dsppkg" else r for r in rows])))
        raise AssertionError("작은 dsppkg 는 거부해야 함")
    except ValueError as e:
        assert "보다 작음" in str(e), e
    for bad, text in ((b"\xFF" * us.PT_SIZE, "비어 있음"), (b"hello" * 100, "파티션 표가 아님"), (b"\xAA\x51" + bytes(30), "파티션 표가 아님")):
        try:
            us.parse_partition_table(bad)
            raise AssertionError("거부해야 함")
        except ValueError as e:
            assert text in str(e), e
    for mod, text in (({"dspfw": ("app", "ota_0")}, "형식이 예상과 다름"), ({"dspfw": ("data", "0x40", "0x3A0010")}, "4KB 경계")):
        rr = [([r[0], mod[r[0]][0], mod[r[0]][1], mod[r[0]][2] if len(mod[r[0]]) > 2 else r[3]] + r[4:]) if r[0] in mod else r for r in rows]
        try:
            us.find_dspfw_partition(us.parse_partition_table(table_bin(rr)))
            raise AssertionError("거부해야 함")
        except ValueError as e:
            assert text in str(e), e
    print("partition table parser ok")

    # ── 2-1. 보관 슬롯 머리·선택 규칙 (브리지 dsp_pkg.c 와 같음) ──
    def head(state, seq, pid="0123456789abcdef", ver="1.0", crc=0x1234):
        fl = {"STAGED": 0xFFFFFFFE, "APPLIED": 0xFFFFFFFC, "OLD": 0xFFFFFFF8, "PARTIAL": 0xFFFFFFFF}[state]
        b = bytearray(us.build_pkg_slot(b"x" * 100, crc, seq, pid, ver)[:us.PKG_HDR_SECTOR])
        struct.pack_into("<I", b, 8, fl)
        return bytes(b)
    E = b"\xFF" * us.PKG_HDR_SECTOR
    sl = us.resolve_slots([E, E])
    assert sl == [None, None] and us.choose_slot(sl) == (0, 1)
    sl = us.resolve_slots([head("APPLIED", 4), E])
    assert sl[0]["state"] == "APPLIED" and us.choose_slot(sl) == (1, 5)
    sl = us.resolve_slots([head("OLD", 4), head("APPLIED", 5)])
    assert us.choose_slot(sl) == (0, 6)
    sl = us.resolve_slots([head("STAGED", 7), head("STAGED", 3)])                           # 둘 다 아님 → seq 작은 쪽
    assert us.choose_slot(sl) == (1, 8)
    sl = us.resolve_slots([head("APPLIED", 9), head("APPLIED", 8)])                         # 전원 차단: seq 큰 쪽이 현재
    assert [x["state"] for x in sl] == ["APPLIED", "OLD"] and us.choose_slot(sl) == (1, 10)
    sl = us.resolve_slots([head("PARTIAL", 12), head("STAGED", 2)])                         # 쓰다 만 슬롯은 빈 슬롯
    assert sl[0] is None and us.choose_slot(sl) == (0, 3)
    sl = us.resolve_slots([b"DSPPKG1\0" + bytes(120), E])                                   # 필드가 이상하면 빈 슬롯
    assert sl[0] is None
    assert pr.pick_slot(us.resolve_slots([head("STAGED", 3), head("APPLIED", 2)]), "auto") == 1
    assert pr.pick_slot(us.resolve_slots([head("STAGED", 3), head("STAGED", 5)]), "auto") == 1
    assert pr.pick_slot(us.resolve_slots([head("STAGED", 3), head("APPLIED", 2)]), "staged") == 0
    for slots, want, text in (([E, E], "auto", "비어 있음"), ([head("OLD", 1), E], "auto", "OLD"), ([head("STAGED", 1), E], "applied", "APPLIED"),
                              ([E, head("STAGED", 1)], "0", "비어 있음")):
        try:
            pr.pick_slot(us.resolve_slots(slots), want)
            raise AssertionError("거부해야 함")
        except ValueError as e:
            assert text in str(e), e
    for bad in ((b"", 1, "0123456789abcdef"), (b"x" * (us.PKG_MAX + 1), 1, "0123456789abcdef"), (b"x", 1, "0123456789ABCDEF")):
        try:
            us.build_pkg_slot(bad[0], 0, bad[1], bad[2], "1.0")
            raise AssertionError("거부해야 함")
        except ValueError:
            pass
    print("package slot header / choice rules ok")

    # ── 3. esptool 탐색 순서 ──
    assert us.find_esptool('python -m "my esptool" --x') == ["python", "-m", "my esptool", "--x"]
    with tempfile.TemporaryDirectory() as d:
        saved = (us.importlib.util.find_spec, dict(os.environ))
        try:
            us.importlib.util.find_spec = lambda name: object()
            assert us.find_esptool() == [sys.executable, "-m", "esptool"]               # 모듈이 있으면 그것
            us.importlib.util.find_spec = lambda name: None
            ard = os.path.join(d, "Arduino15", "packages", "esp32", "tools", "esptool_py")
            for v in ("4.5.1", "4.10.0"):
                os.makedirs(os.path.join(ard, v))
                write(os.path.join(ard, v, "esptool.exe"), b"")
            os.environ["LOCALAPPDATA"] = d
            assert us.find_esptool() == [os.path.join(ard, "4.10.0", "esptool.exe")]    # Arduino 코어 내장, 가장 새 버전
            os.environ.pop("LOCALAPPDATA")
            os.makedirs(os.path.join(d, "bin"))
            fake = write(os.path.join(d, "bin", "esptool.py"), b"#!/bin/sh\n")
            os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)
            os.environ["PATH"] = os.path.join(d, "bin")
            assert us.find_esptool() == [fake]                                           # PATH
            os.environ["PATH"] = os.path.join(d, "none")
            try:
                us.find_esptool()
                raise AssertionError("esptool 이 없으면 실패해야 함")
            except ValueError as e:
                assert "pip install esptool" in str(e)
        finally:
            us.importlib.util.find_spec = saved[0]
            os.environ.clear()
            os.environ.update(saved[1])
    print("esptool discovery ok")

    with tempfile.TemporaryDirectory() as d:
        P = lambda n: os.path.join(d, n)
        golden_hex, golden_fw = os.path.join(HT, "golden", "app_golden.hex"), read(os.path.join(HT, "golden", "app_golden.dspfw"))
        fake_hex = dp.write_fake_hex2000(d, golden_hex)
        write(P("AGV_Main.out"), dp.synth_coff_out(TS, pad=200))
        write(P("AGV_Main.map"), dp.synth_map("AGV_Main.out", dp.ctime_utc(TS + 9 * 3600)))     # 링크 PC 가 +9시간대

        # ── 4. bundle (CLI) → info → verify, 기준 .dspfw 와 같은 바이트 ──
        _, err = run([PACK, "bundle", P("AGV_Main.out"), "--map", P("AGV_Main.map"), "--ver", "1.2.3", "--hex2000", fake_hex,
                      "--build", "1700000000", "-o", P("pkg.dsppkg")])
        assert "link_check time" in err and "ver 1.2.3" in err, err
        res = dp.verify_package(P("pkg.dsppkg"))
        assert res["dspfw_bytes"] == golden_fw, "패키지 안의 .dspfw 가 기준 이미지와 다름"
        out, _ = run([PACK, "verify", P("pkg.dsppkg")])
        assert out.startswith("ok:") and res["pkg_id"] in out, out
        out, _ = run([PACK, "info", P("pkg.dsppkg")])
        assert res["pkg_id"] in out and "AGV_Main.out" in out and "-- dspfw --" in out and "app_crc32" in out, out
        out, _ = run([PACK, "info", os.path.join(HT, "golden", "app_golden.dspfw")])        # 맨 .dspfw 도 계속 됨
        assert "app_crc32" in out and "package" not in out
        # 기본 출력 이름: <이름>_<ver>.dsppkg 가 .out 옆에
        run([PACK, "bundle", P("AGV_Main.out"), "--map", P("AGV_Main.map"), "--ver", "1.2.3", "--hex2000", fake_hex])
        assert os.path.isfile(P("AGV_Main_1.2.3.dsppkg"))
        # 실패는 error: 와 종료 코드 1
        _, err = run([PACK, "bundle", P("AGV_Main.out"), "--map", P("AGV_Main.map"), "--ver", "1.2.3", "--hex2000", P("nothing.py")], ok=False)
        assert err.startswith("error:") and "--hex2000" in err, err
        write(P("other.map"), dp.synth_map("AGV_Main.out", dp.ctime_utc(TS + 5000)))
        _, err = run([PACK, "bundle", P("AGV_Main.out"), "--map", P("other.map"), "--ver", "1.2.3", "--hex2000", fake_hex, "-o", P("bad.dsppkg")], ok=False)
        assert "링크 시각이 다름" in err and not os.path.exists(P("bad.dsppkg")), err
        _, err = run([PACK, "bundle", P("AGV_Main.out"), "--map", P("other.map"), "--ver", "1.2.3", "--hex2000", fake_hex, "-o", P("skip.dsppkg"),
                      "--no-link-check"])
        assert "warning" in err and "skipped" in err, err
        out, _ = run([PACK, "verify", P("skip.dsppkg")])
        assert "link_check skipped" in out
        write(P("tampered.dsppkg"), read(P("pkg.dsppkg"))[:-30] + bytes(30))
        _, err = run([PACK, "verify", P("tampered.dsppkg")], ok=False)
        assert err.startswith("error:"), err
        print("bundle / info / verify CLI ok (golden .dspfw reproduced inside the package)")

        # ── 5. USB 스테이징 머리 (--write-blob) ──
        run([STAGE, "-", P("pkg.dsppkg"), "--write-blob", P("blob.bin")])
        blob, dspfw = read(P("blob.bin")), res["dspfw_bytes"]
        f = lambda n: blob[fields[n][0]:fields[n][0] + fields[n][1]]
        u32 = lambda n: struct.unpack("<I", f(n))[0]
        info = dp.unpack(golden_fw)
        assert len(blob) == 64 + len(dspfw) and blob[64:] == dspfw
        assert f("magic") == b"DSPSTG1\0"
        assert u32("job") == 0
        assert u32("file_len") == len(dspfw) == len(golden_fw)
        assert f("file_crc") == dspfw[12:16] and u32("file_crc") == dp.crc32_final(dp.crc32_bytes(dspfw[32:]))
        assert u32("app_crc") == info["app_crc"] == res["dspfw_info"]["app_crc"]
        assert f("ver") == b"1.2.3" + bytes(11)
        with zipfile.ZipFile(P("pkg.dsppkg")) as z:
            pid = hashlib.sha256(z.read("manifest.json")).hexdigest()[:16]
        assert pid == res["pkg_id"] and f("rsvd") == pid.encode("ascii") + bytes(8)
        assert us.build_stage_blob(dspfw, "1.2.3", info["app_crc"], pid) == blob
        # 보관 슬롯 이미지 (슬롯 0, seq 1): 머리 섹터 + 패키지 원본
        pkg_bytes, slot_img = read(P("pkg.dsppkg")), read(P("blob.pkg.bin"))
        g = lambda n: slot_img[pf[n][0]:pf[n][0] + pf[n][1]]
        g32 = lambda n: struct.unpack("<I", g(n))[0]
        assert len(slot_img) == 0x1000 + len(pkg_bytes) and slot_img[0x1000:] == pkg_bytes and slot_img[128:0x1000] == b"\xFF" * (0x1000 - 128)
        assert g("magic") == b"DSPPKG1\0" and g32("flags") == 0xFFFFFFFE and g32("seq") == 1 and g32("pkg_size") == len(pkg_bytes)
        assert g32("job") == 0 and g("dspfw_crc") == dspfw[12:16] and g("pkg_id") == pid.encode() + b"\0"
        assert g("ver") == b"1.2.3" + bytes(11) and g("sha256") == hashlib.sha256(pkg_bytes).digest() and g("rsvd") == b"\xFF" * 35
        x = us.parse_pkg_slot(slot_img)
        assert (x["state"], x["id"], x["ver"], x["size"], x["dspfw_crc"]) == ("STAGED", pid, "1.2.3", len(pkg_bytes), u32("file_crc"))
        print("stage header ok (byte-exact vs stage_hdr_t, pkg id in rsvd) + package slot image (byte-exact vs pkg_hdr_t)")

        # ── 6. 거부: 맨 .dspfw, 패키지가 아닌 파일, 깨진 패키지, 포트 없음 ──
        _, err = run([STAGE, "-", os.path.join(HT, "golden", "app_golden.dspfw"), "--write-blob", P("x.bin")], ok=False)
        assert "dsp_pack.py bundle" in err and "패키지" in err and not os.path.exists(P("x.bin")), err
        write(P("note.txt"), b"hello")
        _, err = run([STAGE, "-", P("note.txt"), "--write-blob", P("x.bin")], ok=False)
        assert "패키지" in err
        _, err = run([STAGE, "-", P("tampered.dsppkg"), "--write-blob", P("x.bin")], ok=False)
        assert err.startswith("error:") and not os.path.exists(P("x.bin"))
        _, err = run([STAGE, "-", P("pkg.dsppkg")], ok=False)
        assert "포트" in err
        print("reject bare .dspfw / non-package / tampered ok")

        # ── 7. 가짜 esptool: 호출 순서·인자 ──
        fake_esp = write(P("fake_esptool.py"), b'''import json, os, shutil, sys
a = sys.argv[1:]
d = os.environ["FAKE_ESPTOOL_DIR"]
cmd = next(x for x in a if x in ("read_flash", "write_flash"))
n = len(os.listdir(d))
with open(os.path.join(d, "call%d.json" % n), "w") as f:
    json.dump(a, f)
if os.environ.get("FAKE_ESPTOOL_FAIL") == cmd:
    sys.exit(2)
i = a.index(cmd)
if cmd == "read_flash":
    off, size = int(a[i + 1], 0), int(a[i + 2], 0)
    if off == 0x8000:
        shutil.copyfile(os.environ["FAKE_ESPTOOL_TABLE"], a[i + 3])
    else:
        dump = os.environ.get("FAKE_ESPTOOL_DSPPKG")       # dsppkg partition (0x400000) dump
        data = b""
        if dump and off >= 0x400000:
            with open(dump, "rb") as f:
                f.seek(off - 0x400000)
                data = f.read(size)
        with open(a[i + 3], "wb") as f:
            f.write(data.ljust(size, bytes([255])))
else:
    for j in range(i + 1, len(a), 2):
        shutil.copyfile(a[j + 1], os.path.join(d, "w%d_%s.bin" % (n, a[j])))
''')
        esp = " ".join(shlex.quote(x) for x in [sys.executable, fake_esp])
        pre = ["--chip", "esp32s3", "-p", "COM9", "-b", "921600", "--before", "default_reset"]

        def stage(table, extra=(), fail=None, ok=True, args=("COM9",), dump=None, tool=STAGE, pkg=True):
            cd = tempfile.mkdtemp(dir=d)
            env = {"FAKE_ESPTOOL_DIR": cd, "FAKE_ESPTOOL_TABLE": write(P("table.bin"), table)}
            if dump is not None:
                env["FAKE_ESPTOOL_DSPPKG"] = write(P("dsppkg_dump.bin"), dump)
            if fail:
                env["FAKE_ESPTOOL_FAIL"] = fail
            out, err = run([tool, *args, *([P("pkg.dsppkg")] if pkg else []), "--esptool", esp, *extra], env=env, ok=ok)
            calls = [json.load(open(os.path.join(cd, n))) for n in sorted(os.listdir(cd), key=lambda n: (len(n), n)) if n.startswith("call")]
            return calls, cd, out + err

        rd = lambda off, size: lambda c: pre + ["--after", "no_reset", "read_flash", off, size, c[-1]]
        # a. 보드의 표·슬롯 머리 2개를 읽고, write_flash 한 번으로 dspfw 스테이징 + 보관 슬롯 (설정 유지). 빈 보드 → 슬롯 0, seq 1
        calls, cd, msg = stage(table_bin(rows))
        assert [c[:-1] for c in calls[:3]] == [rd("0x8000", "0xc00")(c)[:-1] for c in calls[:1]] + \
            [rd("0x400000", "0x1000")(calls[1])[:-1], rd("0xa00000", "0x1000")(calls[2])[:-1]], calls
        assert calls[3] == pre + ["--after", "hard_reset", "write_flash", "0x3a0000", calls[3][-3], "0x400000", calls[3][-1]], calls
        assert len(calls) == 4 and read(os.path.join(cd, "w3_0x3a0000.bin")) == blob and read(os.path.join(cd, "w3_0x400000.bin")) == slot_img
        assert "dsp apply" in msg and "staged=1.2.3(job 0)" in msg and "1.1.0" in msg and "FLASH_GUIDE" in msg and "설정" in msg, msg
        assert f"staged_pkg={pid}" in msg and "dsp pkg" in msg and "슬롯 0" in msg, msg
        #    슬롯 0 이 APPLIED(seq 4) → 슬롯 1 (0xA00000) 에 seq 5
        dump = bytearray(b"\xFF" * (us.PKG_SLOT_SIZE + us.PKG_HDR_SECTOR))
        dump[:us.PKG_HDR_SECTOR] = head("APPLIED", 4, "aaaaaaaaaaaaaaaa", "1.1.0")
        calls, cd, msg = stage(table_bin(rows), dump=bytes(dump))
        assert calls[3][-4:-2] == ["0x3a0000", calls[3][-3]] and calls[3][-2] == "0xa00000", calls[3]
        w = read(os.path.join(cd, "w3_0xa00000.bin"))
        assert us.parse_pkg_slot(w)["seq"] == 5 and w[0x1000:] == pkg_bytes and "APPLIED id=aaaaaaaaaaaaaaaa" in msg, msg
        # b. --bridge: 표·슬롯을 읽지 않고, 한 번의 write_flash 로 전체 이미지 + 스테이징 + 슬롯 0 (seq 1)
        merged = bytearray(b"\xE9" + bytes(0x8000 - 1)) + table_bin(rows) + bytes(0x20000)
        write(P("merged.bin"), bytes(merged))
        calls, cd, msg = stage(b"", ["--bridge", P("merged.bin"), "--baud", "115200"])
        assert calls == [["--chip", "esp32s3", "-p", "COM9", "-b", "115200", "--before", "default_reset", "--after", "hard_reset",
                          "write_flash", "0x0", P("merged.bin"), "0x3a0000", calls[0][-3], "0x400000", calls[0][-1]]], calls
        assert read(os.path.join(cd, "w0_0x0.bin")) == bytes(merged) and read(os.path.join(cd, "w0_0x3a0000.bin")) == blob
        assert read(os.path.join(cd, "w0_0x400000.bin")) == slot_img
        assert "지워" in msg and "dsp apply" in msg, msg
        # c. 0.5.0 이전 배치: 쓰지 않고 --bridge 안내 / 0.5.0~0.8.x 배치(dsppkg 없음): 다시 굽기 안내
        calls, cd, msg = stage(table_bin(old), ok=False)
        assert [c[c.index("read_flash") if "read_flash" in c else 0] for c in calls] == ["read_flash"], calls
        assert "--bridge" in msg and "0.5.0" in msg and "지워" in msg, msg
        calls, cd, msg = stage(table_bin(old8), ok=False)
        assert len(calls) == 1 and "dsppkg" in msg and "0.9.0" in msg and "--bridge" in msg and "4개 파일" in msg and "16MB" in msg, msg
        #    dspfw·dsppkg 가 없는 전체 이미지, 전체 이미지가 아닌 파일
        write(P("merged_old.bin"), b"\xE9" + bytes(0x8000 - 1) + table_bin(old))
        calls, _, msg = stage(b"", ["--bridge", P("merged_old.bin")], ok=False)
        assert calls == [] and "dspfw" in msg, msg
        write(P("merged_08.bin"), b"\xE9" + bytes(0x8000 - 1) + table_bin(old8))
        calls, _, msg = stage(b"", ["--bridge", P("merged_08.bin")], ok=False)
        assert calls == [] and "dsppkg" in msg and "0.9.0" in msg, msg
        calls, _, msg = stage(b"", ["--bridge", P("note.txt")], ok=False)
        assert calls == [] and "전체 이미지가 아님" in msg, msg
        # d. 표가 아닌 데이터, 작은 파티션, esptool 실패
        calls, _, msg = stage(b"hello" * 100, ok=False)
        assert len(calls) == 1 and "파티션 표가 아님" in msg, msg
        small = [(r[:4] + ["0x200"] + r[5:]) if r[0] == "dspfw" else r for r in rows]
        calls, _, msg = stage(table_bin(small), ok=False)
        assert len(calls) == 3 and "보다 큼" in msg, msg
        calls, _, msg = stage(table_bin(rows), fail="write_flash", ok=False)
        assert len(calls) == 4 and "esptool 실패" in msg and "dsp apply" not in msg, msg
        calls, _, msg = stage(table_bin(rows), fail="read_flash", ok=False)
        assert len(calls) == 1 and "esptool 실패" in msg, msg
        print("fake esptool flows ok (read table + slot heads + one write_flash / slot choice / --bridge / old layouts / failures)")

        # ── 8. dsp_pkg_read.py: 보드(가짜 esptool)·덤프에서 꺼내기 ──
        dump = bytearray(b"\xFF" * (2 * us.PKG_SLOT_SIZE))
        dump[:len(slot_img)] = slot_img                                                       # 슬롯 0: STAGED (USB 스테이징)
        other = us.build_pkg_slot(read(P("skip.dsppkg")), 0x5555, 7, dp.verify_package(P("skip.dsppkg"))["pkg_id"], "1.2.3", job=42)
        other = bytearray(other)
        struct.pack_into("<I", other, 8, 0xFFFFFFFC)                                          # 슬롯 1: APPLIED
        dump[us.PKG_SLOT_SIZE:us.PKG_SLOT_SIZE + len(other)] = other
        write(P("dump.bin"), bytes(dump))
        out, err = run([READ, "-", "--from-dump", P("dump.bin"), "-o", P("got_auto.dsppkg")])
        assert read(P("got_auto.dsppkg")) == read(P("skip.dsppkg")) and "slot 1 APPLIED" in out and "job 42" in out, out + err
        out, err = run([READ, "-", "--from-dump", P("dump.bin"), "-o", P("got0.dsppkg"), "--slot", "staged"])
        assert read(P("got0.dsppkg")) == pkg_bytes and pid in out and "link_check time" in out and "AGV_Main.out" in out, out
        #    기본 이름: <.out 이름>_<ver>_<ID 8자>.dsppkg (현재 폴더)
        cwd = os.getcwd()
        try:
            os.chdir(d)
            run([READ, "-", "--from-dump", P("dump.bin"), "--slot", "0"])
        finally:
            os.chdir(cwd)
        assert read(P(f"AGV_Main_1.2.3_{pid[:8]}.dsppkg")) == pkg_bytes
        #    flash 의 패키지 손상 → sha256 불일치, 파일을 쓰지 않음
        bad = bytearray(dump)
        bad[0x1000 + 100] ^= 1
        write(P("dump_bad.bin"), bytes(bad))
        _, err = run([READ, "-", "--from-dump", P("dump_bad.bin"), "--slot", "0", "-o", P("x.dsppkg")], ok=False)
        assert "sha256" in err and "손상" in err and not os.path.exists(P("x.dsppkg")), err
        _, err = run([READ, "-", "--from-dump", P("dump_bad.bin"), "--slot", "applied", "-o", P("y.dsppkg")])   # 다른 슬롯은 정상
        _, err = run([READ, "-", "--from-dump", write(P("empty.bin"), b"\xFF" * 0x1000), "-o", P("x.dsppkg")], ok=False)
        assert "비어 있음" in err, err
        #    가짜 esptool: 표 → 슬롯 머리 2개 → 고른 슬롯의 패키지 바이트 (읽기만)
        calls, _, msg = stage(table_bin(rows), ["-o", P("got_board.dsppkg")], dump=bytes(dump), tool=READ, pkg=False)
        assert [c[c.index("read_flash") + 1:c.index("read_flash") + 3] for c in calls] == [
            ["0x8000", "0xc00"], ["0x400000", "0x1000"], ["0xa00000", "0x1000"], ["0xa01000", hex(len(other) - 0x1000)]], calls
        assert all("write_flash" not in c and c[c.index("--after") + 1] == "no_reset" for c in calls)
        assert read(P("got_board.dsppkg")) == read(P("skip.dsppkg")) and "wrote" in msg, msg
        calls, _, msg = stage(table_bin(old8), dump=bytes(dump), tool=READ, pkg=False, ok=False)
        assert len(calls) == 1 and "dsppkg" in msg and "0.9.0" in msg, msg
        _, err = run([READ, "-"], ok=False)
        assert "포트" in err, err
        print("dsp_pkg_read.py ok (dump / board read via fake esptool / slot choice / corrupt / old layout)")

    print("usb stage tests: all passed")


if __name__ == "__main__":
    main()
