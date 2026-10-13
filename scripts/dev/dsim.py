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


def fifo_idle():
    """Samsung's dsim_reg_header_fifo_is_empty: FIFO status bit 10."""
    for _ in range(5000):
        if rd(0x68) & 0x400:
            return True
        time.sleep(0.00002)
    return False


def send(dt, data=b""):
    fifo_idle()
    _send(dt, data)
    fifo_idle()
    time.sleep(0.001)


def _send(dt, data=b""):
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
# The 1440x2960 row of star_a3_s0_pps_table (the packet's own bytes are the 1080x2220 template:
# sending those gives static). Slices are 720x40.
PPS = bytes.fromhex(
    "1100008930800b9005a0002802d002d0020002680020046c000a000c027701e9180010f0030c2000060b0b330e1c2a3846"
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
    cmd(0x2A, 0x00, 0x00, 0x05, 0x9F); cmd(0x2B, 0x00, 0x00, 0x0B, 0x8F)   # caset, paset (1440x2960 rows)
    cmd(*L1); cmd(0x1A, 0x1F, 0x00, 0x00, 0x00, 0x00); cmd(0x35, 0x00); cmd(*L1X)   # set_area, te_on
    cmd(*L2); cmd(0xED, 0x4C); cmd(*L3)                                    # err_fg
    cmd(0xC5, 0x0D, 0x10, 0xB4, 0x3E, 0x01); cmd(*L3X)                     # ffc
    cmd(0xB9, 0x01, 0xB0, 0x81, 0x09, 0x00, 0x00, 0x00, 0x11, 0x03)        # tsp_hsync
    cmd(*L2X)                                         # (isc F6 43 sits at register offset 3: not sent)


def up():
    wait(5000)
    cmd(*L1); send(0x07, b"\x01"); send(0x0A, PPS); cmd(*L1X)             # DSC on, PPS
    cmd(0x11)                                                              # sleep out
    wait(10000)
    reinit()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)                                         # display-on-seq


RXFIFO, FIFOSTAT = 0x60, 0x68
RX_DONE, RX_EMPTY = 1 << 18, 1 << 12


def read(reg, n):
    """A DCS read as Samsung's dsim_read_data does it: set the return size, ask with a
    bus turn-around, then take the answer out of the receive FIFO. None if the panel
    did not answer."""
    while not rd(FIFOSTAT) & RX_EMPTY:                # anything left over
        rd(RXFIFO)
    wr(INTSRC, RX_DONE | PH_EMPTY)
    wr(PKTHDR, 0x37 | (n & 0xFF) << 8)                # set maximum return packet size
    for _ in range(2000):
        if rd(INTSRC) & PH_EMPTY:
            break
        time.sleep(0.00001)
    wr(INTSRC, PH_EMPTY)
    wr(PKTHDR, 0x06 | reg << 8 | 1 << 24)             # DCS read, with the turn-around
    for _ in range(300):
        if rd(INTSRC) & RX_DONE:
            break
        time.sleep(0.001)
    else:
        return None
    h = rd(RXFIFO)
    t = h & 0xFF
    if t in (0x1C, 0x1A):                             # long answer: length, then the bytes
        ln = h >> 8 & 0xFFFF
        out = b""
        for _ in range((ln + 3) // 4):
            out += rd(RXFIFO).to_bytes(4, "little")
        out = out[:ln]
    elif t in (0x11, 0x21):
        out = bytes([h >> 8 & 0xFF])
    elif t in (0x12, 0x22):
        out = bytes([h >> 8 & 0xFF, h >> 16 & 0xFF])
    else:
        out = None                                    # 0x02: the panel's error report
    while not rd(FIFOSTAT) & RX_EMPTY:
        rd(RXFIFO)
    return out


def dump():
    """Every readable register, for comparing the panel before and after a sleep."""
    for attempt in range(10):                         # the keys must have taken, or most registers read as 0
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        k = read(0xF0, 2)
        k3 = read(0xFC, 2)
        if k == b"\x5a\x5a" and k3 == b"\x5a\x5a":
            break
        time.sleep(0.05)
    print("# unlock after %d tries: f0=%s fc=%s" % (attempt + 1, k.hex() if k else "-", k3.hex() if k3 else "-"))
    for reg in list(range(0x04, 0x10)) + [0x1A, 0x2A, 0x2B, 0x35, 0x51, 0x53, 0x55, 0x9D, 0x9E] + list(range(0xA0, 0x100)):
        for _ in range(4):                            # a read sometimes gets no answer: ask again
            v = read(reg, 60)
            if v is not None and len(v) == 60:
                break
            time.sleep(0.02)
        print("%02x %s" % (reg, v.hex() if v is not None else "-"))
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def deep():
    """dsim.py deep REG [REG ...]: read a register 240 bytes deep, 60 at a time, with the
    panel's offset register (B0)."""
    for _ in range(10):
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        if read(0xF0, 2) == b"\x5a\x5a" and read(0xFC, 2) == b"\x5a\x5a":
            break
    for h in sys.argv[2:]:
        reg = int(h, 16)
        out = b""
        for off in range(0, 240, 60):
            for _ in range(4):
                cmd(0xB0, off)
                v = read(reg, 60)
                if v is not None and len(v) == 60:
                    break
                time.sleep(0.02)
            out += v if v else b"?" * 60
        cmd(0xB0, 0)
        print("%02x %s" % (reg, out.hex()))
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def peek():
    """dsim.py r REG [REG ...]: the first 16 bytes of each register."""
    for _ in range(10):
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        if read(0xF0, 2) == b"\x5a\x5a" and read(0xFC, 2) == b"\x5a\x5a":
            break
    for h in sys.argv[2:]:
        v = None
        for _ in range(4):
            v = read(int(h, 16), 16)
            if v:
                break
        print("%s %s" % (h, v.hex() if v else "-"))
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def blind():
    """dsim.py wo HEX [HEX ...]: write registers with no reads at all (a read disturbs the
    panel: it clears the first byte of some registers). Keys and writes are sent three times."""
    for _ in range(3):
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.03)
    for _ in range(3):
        for h in sys.argv[2:]:
            cmd(*bytes.fromhex(h))
        time.sleep(0.03)
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def poke():
    """dsim.py w HEX [HEX ...]: write registers (first byte is the register) under the level 2 and 3 keys."""
    for _ in range(10):
        cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        if read(0xF0, 2) == b"\x5a\x5a":
            break
    for h in sys.argv[2:]:
        cmd(*bytes.fromhex(h))
    for h in sys.argv[2:]:
        b = bytes.fromhex(h)
        print("%02x now %s" % (b[0], (read(b[0], len(b) - 1) or b"").hex()))
    cmd(*L3X); cmd(*L2X)


def up_min():                                   # only the keyed sleep out and display on
    cmd(*L1); cmd(0x11); cmd(*L1X)
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)


def up_reinit():                                      # keyed sleep out, the re-init, display on; no DSC/PPS
    cmd(*L1); cmd(0x11); cmd(*L1X)
    wait(10000)
    reinit()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)


def set_bl():                                         # set-bl-seq with the table's default values
    cmd(*L2)
    # (gamma CA and IRC B8 are placeholders in the kernel, filled from the panel's own calibration: not sent)
    cmd(0xB1, 0x00, 0x0C)                                                  # aor
    cmd(0xB5, 0x19, 0xDC, 0x0A); cmd(0xB5, 0x0C)                           # tset_mps_elvss, elvss_temp
    cmd(0xF4, 0xEB, 0x17)                                                  # vgh_vint
    cmd(0xB4, 0x00, 0x44, 0x80, 0x65, 0x26, 0x00); cmd(0xB4, 0x20)         # acl control, dim frame
    cmd(0x55, 0x00)                                                        # acl off
    cmd(0xF7, 0x03)                                                        # gamma_update_enable
    cmd(*L2X)


def up_bl():                                          # keyed wake, re-init, brightness block, display on
    cmd(*L1); cmd(0x11); cmd(*L1X)
    wait(10000)
    reinit()
    set_bl()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)


{"down": down, "up": up, "resume": reinit, "up_min": up_min, "up_reinit": up_reinit, "up_bl": up_bl,
 "dump": dump, "w": poke, "deep": deep, "r": peek, "wo": blind}[sys.argv[1]]()
