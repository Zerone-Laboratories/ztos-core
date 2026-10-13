#!/usr/bin/env python3
# dsim.py: send Samsung's S6E3HA8 (starlte) panel sequences straight to the DSIM, from /dev/mem.
# The command bytes come out of the stock kernel's panel tables (star_a3_s0_*): the sleep-out
# ("init-seq") re-sends DSC, PPS, scaler, area, TE and the rest, and the access keys wrap them.
# Run as root with the screen off.
#   dsim.py down      display off, sleep in   (Samsung's display-off-seq and exit-seq)
#   dsim.py up        sleep out, re-init, display on   (then "screen on" from the shell)
#   dsim.py resume    only the re-init, for after a bare sleep out
import mmap
import os
import struct
import sys
import time

PKTHDR, PAYLOAD, INTSRC = 0x58, 0x5C, 0x50
PH_EMPTY = 1 << 28
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, offset=0x16080000)


def rd(o):
    return struct.unpack_from("<I", m, o)[0]


def wr(o, v):
    struct.pack_into("<I", m, o, v)


def send(dt, data=b""):
    wr(INTSRC, PH_EMPTY)
    if dt in (0x05, 0x15, 0x07):                      # short packets
        d = data + b"\0\0"
        wr(PKTHDR, dt | d[0] << 8 | d[1] << 16)
    else:                                             # long: the payload words first, then the header
        for i in range(0, len(data), 4):
            wr(PAYLOAD, int.from_bytes(data[i:i + 4].ljust(4, b"\0"), "little"))
        wr(PKTHDR, dt | (len(data) & 0xFF) << 8 | (len(data) >> 8) << 16)
    for _ in range(2000):
        if rd(INTSRC) & PH_EMPTY:
            return
        time.sleep(0.00001)
    print("panel did not take", hex(dt), data[:4].hex())


def cmd(*b):                                          # a DCS write: short or long as its length says
    send(0x05 if len(b) == 1 else 0x15 if len(b) == 2 else 0x39, bytes(b))


def wait(us):
    time.sleep(us / 1e6)


L1, L1X = (0x9F, 0xA5, 0xA5), (0x9F, 0x5A, 0x5A)
L2, L2X = (0xF0, 0x5A, 0x5A), (0xF0, 0xA5, 0xA5)
L3, L3X = (0xFC, 0x5A, 0x5A), (0xFC, 0xA5, 0xA5)
PPS = bytes.fromhex(
    "11000089308008ac0438001e021c021c0200020e002002e30007000c03500364180010f0030c2000060b0b330e1c2a3846"
    "5462697077797b7d7e01020100094009be19fc19fa19f81a381a781ab62af62b342b743b746bf4" + "00" * 49)


PPS = PPS[:128].ljust(128, b"\0")                     # (always 128 bytes)


def down():                                           # display-off-seq, then exit-seq
    cmd(*L1); cmd(0x28); cmd(*L1X)
    wait(20000)
    cmd(*L2); cmd(0xE2, 0x00); cmd(*L2X)              # afc-off-seq
    wait(20000)
    cmd(*L1); cmd(0x10)                               # sleep in (the key stays enabled, as in exit-seq)
    wait(100000)


def reinit():                                         # the init-seq after the sleep out
    cmd(*L2); cmd(0xBA, 0x01, 0x26, 0x08, 0x08, 0xF3); cmd(*L2X)           # scaler
    cmd(0x2A, 0x00, 0x00, 0x05, 0x9F); cmd(0x2B, 0x00, 0x00, 0x09, 0xFF)   # caset, paset
    cmd(*L1); cmd(0x1A, 0x1F, 0x00, 0x00, 0x00, 0x00); cmd(0x35, 0x00); cmd(*L1X)   # set_area, te_on
    cmd(*L2); cmd(0xED, 0x4C); cmd(*L3)                                    # err_fg
    cmd(0xC5, 0x0D, 0x10, 0xB4, 0x3E, 0x01); cmd(*L3X)                     # ffc
    cmd(0xB9, 0x01, 0xB0, 0x81, 0x09, 0x00, 0x00, 0x00, 0x11, 0x03)        # tsp_hsync
    cmd(0xF6, 0x43); cmd(*L2X)                                             # isc


def up():
    wait(5000)
    cmd(*L1); send(0x07, b"\x01"); send(0x0A, PPS); cmd(*L1X)             # DSC on, PPS
    cmd(0x11)                                                              # sleep out
    wait(10000)
    reinit()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)                                         # display-on-seq


{"down": down, "up": up, "resume": reinit}[sys.argv[1]]()
