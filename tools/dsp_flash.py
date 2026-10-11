#!/usr/bin/env python3
"""메인보드(F2808) 부트로더 시리얼 프로그래머 — 벤치용 (브리지 없이 PC 의 RS232 로 직접)

  dsp_flash.py COM5 app.dspfw            # 앱 프로그래밍 (HELLO → ERASE → WRITE → 헤더 → VERIFY → RESET)
  dsp_flash.py COM5 app.dspfw --enter 7  # 먼저 07호기 앱에 F…U000 프레임으로 부트로더 진입 요청
  dsp_flash.py COM5 --hello              # 부트로더 응답·앱 유효성 확인
  dsp_flash.py COM5 --read 0x3E8000 64   # 플래시·OTP 읽기 (진단, 최대 128 워드)
  dsp_flash.py COM5 --query 7            # 앱에 F…V000 버전 조회

프로토콜: dsp/common/bootproto.h, 프레임: docs/DSP_OTA_DESIGN.md 5절. pyserial 필요 (pip install pyserial).
"""
import argparse
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsp_pack  # noqa: E402

try:
    import serial
except ImportError:
    print("pyserial 이 필요합니다: pip install pyserial", file=sys.stderr)
    sys.exit(1)

SOF = b"\xC5\x3A"
CMD_HELLO, CMD_ERASE, CMD_WRITE, CMD_VERIFY, CMD_RESET, CMD_READ = 1, 2, 3, 4, 5, 6
ST = {0: "OK", 1: "BAD_ADDR", 2: "BAD_LEN", 3: "ERASE_FAIL", 4: "PROG_FAIL", 5: "VERIFY_FAIL", 6: "BAD_CMD", 7: "BAD_PAYLOAD"}
BLOCK_WORDS = 128
STX, ETX = b"\x02", b"\x03"


class Boot:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.05)
        # 실행마다 seq 시작값을 바꾼다 — 부트로더는 같은 (seq, cmd, 요청 CRC) 재전송에 이전 응답을 돌려주므로
        # 연속 실행한 두 도구가 같은 seq 로 시작하면 낡은 응답을 받을 수 있다
        self.seq = random.randrange(256)

    def xfer(self, cmd, payload=b"", timeout=1.0, retries=3):
        self.seq = (self.seq + 1) & 0xFF
        body = bytes([cmd, self.seq]) + payload
        pkt = SOF + struct.pack("<H", len(body)) + body + struct.pack("<H", dsp_pack.crc16_bytes(body))
        for attempt in range(retries):
            self.s.reset_input_buffer()
            self.s.write(pkt)
            r = self._recv(cmd | 0x80, self.seq, timeout)
            if r is not None:
                return r
            print(f"  cmd {cmd:#04x} seq {self.seq}: 무응답 (재시도 {attempt + 1})", file=sys.stderr)
        raise RuntimeError(f"cmd {cmd:#04x}: 응답 없음")

    def _recv(self, want_cmd, want_seq, timeout):
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            b = self.s.read(256)
            if not b:
                continue
            buf += b
            while True:
                i = buf.find(SOF)
                if i < 0:
                    buf = buf[-1:] if buf.endswith(b"\xC5") else b""
                    break
                if len(buf) < i + 4:
                    break
                n = struct.unpack_from("<H", buf, i + 2)[0]
                if n < 2 or n > 2 + 6 + BLOCK_WORDS * 2:
                    buf = buf[i + 1:]
                    continue
                if len(buf) < i + 4 + n + 2:
                    break
                body = buf[i + 4:i + 4 + n]
                crc = struct.unpack_from("<H", buf, i + 4 + n)[0]
                buf = buf[i + 6 + n:]
                if crc == dsp_pack.crc16_bytes(body) and body[0] == want_cmd and body[1] == want_seq:
                    return body[2:]
        return None

    def hello(self):
        r = self.xfer(CMD_HELLO)
        if len(r) != 61 or r[0] != 0:
            raise RuntimeError(f"HELLO 실패: {r.hex()}")
        return {
            "proto": r[1], "max_block": r[2] | (r[3] << 8), "app_valid": bool(r[4]),
            "boot_ver": r[5:21].split(b"\0")[0].decode("ascii", "replace"),
            "app_ver": r[21:53].split(b"\0")[0].decode("ascii", "replace"),
            "app_crc": struct.unpack_from("<I", r, 53)[0], "app_len": struct.unpack_from("<I", r, 57)[0],
        }

    def erase(self, mask=0x07):
        r = self.xfer(CMD_ERASE, bytes([mask]), timeout=20)
        if r[0] != 0:
            raise RuntimeError(f"ERASE: {ST.get(r[0], r[0])}")

    def write(self, addr, words):
        pl = struct.pack("<IH", addr, len(words)) + dsp_pack.words_to_le(words)
        r = self.xfer(CMD_WRITE, pl, timeout=1.5)
        if r[0] != 0:
            raise RuntimeError(f"WRITE 0x{addr:X}: {ST.get(r[0], r[0])} @0x{struct.unpack_from('<I', r, 1)[0]:X}")

    def verify(self):
        r = self.xfer(CMD_VERIFY, timeout=3)
        return bool(r[1]), struct.unpack_from("<I", r, 2)[0], struct.unpack_from("<I", r, 6)[0], r[10]

    def reset(self):
        self.xfer(CMD_RESET, b"\0", retries=1)

    def read(self, addr, n):
        r = self.xfer(CMD_READ, struct.pack("<IH", addr, n))
        if r[0] == 1:
            raise RuntimeError("READ: BAD_ADDR — 플래시 0x3E8000~0x3F7FFF·OTP 0x3D7800~0x3D7BFF 만 읽을 수 있음")
        if r[0] != 0:
            raise RuntimeError(f"READ: {ST.get(r[0], r[0])}")
        return list(struct.unpack_from(f"<{n}H", r, 1))

    # ── 앱 프레임 (AGV-ACS 형식) ──
    def app_query(self, agv, cmd, pan="1", msg="1", timeout=2.0):
        body = f"F{pan}{msg}{agv:02d}{cmd}000 ".encode("ascii")
        frame = STX + body + dsp_pack_bcc(body).encode("ascii") + ETX
        self.s.reset_input_buffer()
        self.s.write(frame)
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            buf += self.s.read(64)
            i, j = buf.find(STX), buf.find(ETX)
            if i >= 0 and j > i:
                pl = buf[i + 1:j]
                buf = buf[j + 1:]
                if len(pl) >= 8 and pl[0:1] == b"R" and pl[-1:] == dsp_pack_bcc(pl[:-1]).encode("ascii"):
                    return pl.decode("ascii", "replace")
        return None


def dsp_pack_bcc(payload: bytes) -> str:
    n = sum(payload) & 0x0F
    return chr(ord("0") + n) if n < 10 else chr(ord("A") + n - 10)


def program(b, path, enter_agv=None, pan="1"):
    with open(path, "rb") as f:
        info = dsp_pack.unpack(f.read())
    print(f"image: ver {info['ver']} app_len {info['app_len']} words crc {info['app_crc']:08X} blocks {len(info['blocks'])}")

    if enter_agv is not None:
        print(f"앱에 업데이트 진입 요청 (AGV {enter_agv:02d})...")
        r = b.app_query(enter_agv, "U", pan=pan)
        if r is None:
            print("  응답 없음 — 이미 부트로더일 수 있어 계속 진행")
        elif "UOK" in r:
            print("  OK, 부트로더 기동 대기")
            time.sleep(0.5)
        else:
            raise RuntimeError(f"앱이 거부: {r}")

    h = b.hello()
    print(f"bootloader {h['boot_ver']} proto {h['proto']} app_valid={h['app_valid']} app_ver={h['app_ver']!r}")
    if h["max_block"] < BLOCK_WORDS:
        raise RuntimeError("블록 크기 불일치")
    t0 = time.time()
    print("erase B~D ...", end="", flush=True)
    b.erase()
    print(f" {time.time() - t0:.1f}s")
    t1 = time.time()
    total = sum(len(w) for _, w in info["blocks"])
    done = 0
    for addr, words in info["blocks"]:
        b.write(addr, words)
        done += len(words)
        print(f"\rwrite {done * 100 // total:3d}% ({done}/{total} words)", end="", flush=True)
    print(f"  {time.time() - t1:.1f}s")
    b.write(dsp_pack.APP_HDR_ADDR, info["hdr_words"])
    valid, crc, length, rc = b.verify()
    if not valid or crc != info["app_crc"]:
        raise RuntimeError(f"VERIFY 실패: valid={valid} crc={crc:08X} rc={rc}")
    print(f"verify ok (crc {crc:08X}, {length} words), total {time.time() - t0:.1f}s")
    b.reset()
    print("reset → app")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port")
    p.add_argument("image", nargs="?", help=".dspfw 파일")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--enter", type=int, metavar="AGV", help="프로그래밍 전 앱에 진입 요청 (호기 번호)")
    p.add_argument("--pan", default="1", help="F 프레임 PAN ID (기본 1)")
    p.add_argument("--hello", action="store_true")
    p.add_argument("--read", nargs=2, metavar=("ADDR", "N"))
    p.add_argument("--query", type=int, metavar="AGV", help="앱에 버전 조회")
    a = p.parse_args()
    b = Boot(a.port, a.baud)
    try:
        if a.query is not None:
            r = b.app_query(a.query, "V", pan=a.pan)
            print(r if r else "응답 없음")
        elif a.hello:
            for k, v in b.hello().items():
                print(f"{k:10}: {v:08X}" if k == "app_crc" else f"{k:10}: {v}")
        elif a.read:
            addr, n = int(a.read[0], 0), int(a.read[1], 0)
            words = b.read(addr, n)
            for i in range(0, n, 8):
                print(f"{addr + i:06X}: " + " ".join(f"{w:04X}" for w in words[i:i + 8]))
        elif a.image:
            program(b, a.image, a.enter, a.pan)
        else:
            p.print_help()
    except (RuntimeError, ValueError, OSError) as e:
        print(f"\nerror: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
