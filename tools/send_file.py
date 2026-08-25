#!/usr/bin/env python3
"""Push a file to the Arpile SD card over the USB-serial bridge.

Usage:
  python3 tools/send_file.py put <local_file> [name_on_card] [--baud B]
  python3 tools/send_file.py ls
  python3 tools/send_file.py del <name_on_card>

Protocol mirrors components/file_xfer/file_xfer.c.
"""

import argparse
import os
import re
import sys
import time
import zlib

import serial


def open_port(port: str) -> serial.Serial:
    s = serial.Serial(port, 115200, timeout=0.3)
    s.dtr = False
    s.rts = False
    # opening the port pulses DTR/RTS -> board reboots; wait for the
    # firmware to finish bringing up desktop+wifi+sd (~15 s)
    wait_quiet(s, 40)
    return s


def wait_quiet(s: serial.Serial, max_wait: float):
    """Drain input until the serial line has been silent for ~2.5 s."""
    t0 = time.time()
    last = time.time()
    while time.time() - t0 < max_wait:
        if s.read(4096):
            last = time.time()
        elif time.time() - last > 2.5:
            break


def read_line(s: serial.Serial, timeout=15.0):
    old = s.timeout
    s.timeout = timeout
    line = s.readline().decode(errors="replace").strip()
    s.timeout = old
    return line


def send_cmd(s, cmd: str, want, tries=60, gap=2.0):
    """Send a command until the device gives a usable answer.
    Log noise from (re)boot is ignored; `want` is a regex of accepted
    first-line replies."""
    resp = ""
    for _ in range(tries):
        s.reset_input_buffer()
        s.write(f"{cmd}\n".encode())
        resp = read_line(s, 3.0)
        if re.fullmatch(want, resp):
            return resp
        time.sleep(gap)
    sys.exit(f"no answer to {cmd!r} (last: {resp!r})")


def cmd_ls(s):
    first = send_cmd(s, "ARPFILE ls", r"(\d+ \S+)|END")
    if first != "END":
        print(first)
    for _ in range(4096):
        line = read_line(s, 5.0)
        if not line or line == "END":
            break
        if re.fullmatch(r"(\d+ \S+)|END", line):
            print(line)


def cmd_del(s, name):
    print(send_cmd(s, f"ARPFILE del {name}", r"OK|ERR"))


def cmd_put(s, path, name, baud):
    data = open(path, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    size = len(data)

    hdr = f"ARPFILE put {name} {size} {crc:08x}"
    resp = send_cmd(s, hdr, r"RDY")

    # request faster payload baud (UART bridge only; USJ answers SKIP)
    fast = False
    if resp == "RDY" and baud != 115200:
        s.reset_input_buffer()
        s.write(f"ARPFILE baud {baud}\n".encode())
        r = read_line(s, 2.0)
        if r == "BAUD":
            time.sleep(0.05)
            s.baudrate = baud
            fast = True

    print(f"Sending {size/1048576:.1f} MB as {name} "
          f"@ {'usb' if not fast else baud}")
    t0 = time.time()
    sent = 0
    CH = 8192
    try:
        for off in range(0, size, CH):
            blk = data[off:off + CH]
            s.write(blk)
            sent += len(blk)
            ack = s.read(1)
            if ack != b"K":
                raise IOError(f"bad ack {ack!r} at {sent}")
            done = off / max(size - 1, 1) * 100
            rate = sent / max(time.time() - t0, 0.001) / 1024
            print(f"\r{done:5.1f}%  {rate:6.0f} KB/s", end="", flush=True)
    except (IOError, OSError) as e:
        print(f"\ntransfer error: {e}")

    if fast:
        time.sleep(0.15)
        s.baudrate = 115200

    final = read_line(s, 30.0)
    dt = time.time() - t0
    print()
    if final.startswith("OK"):
        print(f"OK  {final}   ({dt:.0f}s, {size/dt/1024:.0f} KB/s avg)")
    else:
        sys.exit(f"FAILED: device said {final!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=921600)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_put = sub.add_parser("put")
    p_put.add_argument("file")
    p_put.add_argument("name", nargs="?")

    sub.add_parser("ls")

    p_del = sub.add_parser("del")
    p_del.add_argument("name")

    args = ap.parse_args()

    s = open_port(args.port)
    try:
        if args.cmd == "ls":
            cmd_ls(s)
        elif args.cmd == "del":
            cmd_del(s, args.name)
        elif args.cmd == "put":
            name = args.name or os.path.basename(args.file)
            cmd_put(s, args.file, name, args.baud)
    finally:
        s.close()


if __name__ == "__main__":
    main()
