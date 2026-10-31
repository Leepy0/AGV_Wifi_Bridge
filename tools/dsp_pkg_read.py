#!/usr/bin/env python3
"""브리지에 보관된 메인보드 패키지(.dsppkg)를 USB 로 꺼내기 — 원본 복구용 (0.9.0~, 16MB 보드)
AGV 에서 브리지를 떼어 PC 에 꽂으면, 그 AGV 메인보드에서 도는 프로그램의 .dspfw·.out·.map 을 되찾을 수 있다.

  dsp_pkg_read.py COM5                                 # 적용된(APPLIED) 패키지, 없으면 스테이징(STAGED) 패키지를 꺼냄
  dsp_pkg_read.py COM5 -o AGV07.dsppkg --slot staged   # 슬롯 고르기: applied | staged | 0 | 1
  dsp_pkg_read.py - --from-dump dsppkg.bin             # dsppkg 파티션 원시 덤프(12MB)에서 (보드 없이, 시험·오프라인)

  옵션: --baud 921600  --esptool "python -m esptool"

보드는 읽기만 한다 (read_flash). 파티션 표 → 두 슬롯 머리(4KB 씩) → 고른 슬롯의 패키지 바이트 순서로 읽고,
크기·sha256 을 슬롯 머리와 대조한 뒤 dsp_pack.py verify_package 로 전부 검증해 파일로 쓴다.
기본 출력 이름: <.out 이름>_<ver>_<패키지 ID 앞 8자>.dsppkg (현재 폴더). 꺼낸 뒤 dsp_pack.py info 로 내용 확인.
슬롯 배치: docs/DSP_OTA_DESIGN.md 6절, main/dsp_pkg.c. esptool 필요 (pip install esptool).
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsp_pack  # noqa: E402
import dsp_usb_stage as us  # noqa: E402


def pick_slot(slots: list, want: str) -> int:
    """want: applied | staged | 0 | 1 | auto(APPLIED, 없으면 seq 가 가장 큰 STAGED). 없으면 ValueError"""
    if want in ("0", "1"):
        i = int(want)
        if not slots[i]:
            raise ValueError(f"슬롯 {i} 이 비어 있음")
        return i
    if want in ("auto", "applied"):
        cur = [i for i, x in enumerate(slots) if x and x["state"] == "APPLIED"]
        if cur:
            return cur[0]
        if want == "applied":
            raise ValueError("적용된(APPLIED) 패키지가 없음 — --slot staged 또는 0/1 로 다른 슬롯을 꺼낼 수 있음")
    st = [i for i, x in enumerate(slots) if x and x["state"] == "STAGED"]
    if st:
        return max(st, key=lambda i: slots[i]["seq"])
    raise ValueError("꺼낼 패키지가 없음 (" + ("슬롯이 모두 비어 있음" if not any(slots) else
                     "적용·스테이징된 패키지 없음 — 지난(OLD) 패키지는 --slot 0/1 로 꺼낼 수 있음") + ")")


def extract(slot_hdr: dict, data: bytes) -> dict:
    """슬롯 머리와 패키지 바이트 대조 → verify_package 결과"""
    if len(data) != slot_hdr["size"]:
        raise ValueError(f"패키지 크기가 슬롯 머리({slot_hdr['size']} B)와 다름 ({len(data)} B)")
    if hashlib.sha256(data).digest() != slot_hdr["sha256"]:
        raise ValueError("패키지 sha256 이 슬롯 머리와 다름 — 브리지 flash 의 보관 패키지가 손상됨")
    res = dsp_pack.verify_package(data)
    if res["pkg_id"] != slot_hdr["id"]:
        raise ValueError(f"패키지 ID({res['pkg_id']})가 슬롯 머리({slot_hdr['id']})와 다름")
    if res["ver"] != slot_hdr["ver"]:
        raise ValueError(f"패키지 버전({res['ver']})이 슬롯 머리({slot_hdr['ver']})와 다름")
    return res


def default_name(res: dict) -> str:
    stem = os.path.splitext(res["files"]["out"]["name"])[0] or "AGV_Main"
    return f"{stem}_{res['ver']}_{res['pkg_id'][:8]}.dsppkg"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", help="브리지 USB 시리얼 포트 (예: COM5). --from-dump 일 때는 '-'")
    p.add_argument("-o", "--output", metavar="FILE.dsppkg", help="출력 파일 (기본: <이름>_<ver>_<ID 8자>.dsppkg)")
    p.add_argument("--slot", default="auto", choices=("auto", "applied", "staged", "0", "1"),
                   help="꺼낼 슬롯 (기본: applied, 없으면 staged)")
    p.add_argument("--from-dump", metavar="FILE", help="보드 대신 dsppkg 파티션 원시 덤프(오프셋 0 부터)에서 읽기")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--esptool", metavar="CMD", help='esptool 실행 명령 (기본: 자동 탐색). 예: "python -m esptool"')
    a = p.parse_args()
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    try:
        if a.from_dump:
            with open(a.from_dump, "rb") as f:
                dump = f.read()
            heads = [dump[i * us.PKG_SLOT_SIZE:i * us.PKG_SLOT_SIZE + us.PKG_HDR_SECTOR] for i in range(us.PKG_SLOTS)]
            read_pkg = lambda i, n: dump[i * us.PKG_SLOT_SIZE + us.PKG_HDR_SECTOR:i * us.PKG_SLOT_SIZE + us.PKG_HDR_SECTOR + n]
        else:
            if a.port == "-":
                raise ValueError("포트를 지정하세요 (예: COM5). '-' 는 --from-dump 와 함께만 씁니다")
            esptool = us.find_esptool(a.esptool)
            print(f"보드의 파티션 표를 읽습니다 (0x{us.PT_OFFSET:X}, {us.PT_SIZE} B)...", file=sys.stderr)
            table = us.parse_partition_table(us.read_board_table(esptool, a.port, a.baud))
            ppart = us.find_dsppkg_partition(table)
            if ppart is None:
                raise ValueError("dsppkg 파티션이 없음 — 0.9.0 이전 파티션 배치의 브리지라 보관된 패키지가 없습니다")
            print("보관 슬롯 머리를 읽습니다 (2 × 4KB)...", file=sys.stderr)
            heads = us.read_board_slots(esptool, a.port, a.baud, ppart)
            read_pkg = lambda i, n: us.read_board(esptool, a.port, a.baud, ppart["offset"] + i * us.PKG_SLOT_SIZE + us.PKG_HDR_SECTOR,
                                                  n, f"슬롯 {i} 패키지")
        slots = us.resolve_slots(heads)
        for i, x in enumerate(slots):
            print("  " + us.slot_line(i, x), file=sys.stderr)
        i = pick_slot(slots, a.slot)
        x = slots[i]
        print(f"슬롯 {i} 의 패키지를 읽습니다 ({x['size']} B)...", file=sys.stderr)
        data = read_pkg(i, x["size"])
        res = extract(x, data)
        for w in res["warnings"]:
            print(f"warning: {w}", file=sys.stderr)
        out = a.output or default_name(res)
        with open(out, "wb") as f:
            f.write(data)
        print(f"wrote {out}")
        print(f"  slot {i} {x['state']}  id {res['pkg_id']}  ver {res['ver']}  link_check {res['link_check']}  job {x['job']}  seq {x['seq']}")
        print(f"  .out {res['files']['out']['name']}  .map {res['files']['map']['name']}  .dspfw {res['files']['dspfw']['name']}")
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
