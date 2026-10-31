#!/usr/bin/env python3
"""audio-poke.py [up|tone SECONDS|down|dump] -- the VT10101's speaker path by
hand, before any driver: docs/audio.md, "The speaker path, cold to sound".
Runs ON the frame (the userland has python3 and /dev/mem, no STRICT_DEVMEM).

  up      clocks, ownership, audio power, codec DAC, the speaker mixer and
          both amplifiers (the codec's internal PA and GPIO 85), in the
          vendor's order
  tone S  a 440 Hz square wave pushed into the VBC's DA0/DA1 FIFOs by the
          CPU for S seconds (no DMA: the FIFO pacing is guessed, so it will
          not be clean -- any sound proves the path)
  down    the reverse, amplifiers first
  dump    the registers this touches

The SC2713S registers ("ADI x") are written through the ADI's memory
window at 0x40038000 and read through its controller (RD_CMD 0x40030024,
RD_DATA 0x40030028, bit 31 busy), every one read-modify-write. The kernel's
sprd-adi uses the same controller under a hwspinlock this doesn't take: a
read of ours can in principle interleave with one of the kernel's. Fine for
a bench test, not for a driver.
"""
import ctypes
import mmap
import os
import sys
import time

PAGE = 4096
_maps = {}


def _map(addr):
    base = addr & ~(PAGE - 1)
    if base not in _maps:
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        _maps[base] = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
        os.close(fd)
    return _maps[base], addr - base


# one aligned 32-bit load or store per access, through ctypes: struct and
# mmap slices copy with memcpy, whose split/unaligned stores fault on device
# memory (the first try died on an alignment trap at RD_CMD, 2026-10-28)
def _reg(addr):
    m, o = _map(addr)
    return ctypes.c_uint32.from_buffer(m, o)


def rd(addr):
    return _reg(addr).value


def wr(addr, val):
    _reg(addr).value = val & 0xFFFFFFFF


def setb(addr, mask):
    wr(addr, rd(addr) | mask)


def clrb(addr, mask):
    wr(addr, rd(addr) & ~mask)


ADI_CTL = 0x40030000
ADI_WIN = 0x40038000


def adi_rd(off):
    wr(ADI_CTL + 0x24, ADI_WIN + off)
    for _ in range(1000):
        v = rd(ADI_CTL + 0x28)
        if not v & (1 << 31):
            return v & 0xFFFF
    raise RuntimeError(f"ADI read of 0x{off:x} timed out")


def adi_wr(off, val):
    wr(ADI_WIN + off, val & 0xFFFF)


def adi_set(off, mask):
    adi_wr(off, adi_rd(off) | mask)


def adi_clr(off, mask):
    adi_wr(off, adi_rd(off) & ~mask)


def adi_field(off, shift, width, val):
    m = ((1 << width) - 1) << shift
    adi_wr(off, (adi_rd(off) & ~m) | ((val << shift) & m))


def pulse(addr, mask):
    setb(addr, mask)
    time.sleep(0.0001)
    clrb(addr, mask)


B = lambda *bits: sum(1 << b for b in bits)  # noqa: E731

APB_EB0, APB_RST0, SINDRV, VBC_CTRL = 0x402E0000, 0x402E0008, 0x402E0018, 0x402E0020
WPROT_AP, WPROT_CP = 0x402E3040, (0x402E3044, 0x402E3048, 0x402E304C)
PMU_SLEEP, PMU_MEMPD = 0x402B00CC, 0x402B0138
AUD_TOP, AUD_DAC, AUD_SDM0 = 0x40000000, 0x4000000C, 0x40000010
VBC = 0x40020000
GPIO = 512 + 85  # the speaker amp's enable, gpiochip512


def gpio85(v):
    p = f"/sys/class/gpio/gpio{GPIO}"
    if not os.path.isdir(p):
        open("/sys/class/gpio/export", "w").write(str(GPIO))
    open(p + "/direction", "w").write("high" if v else "low")


def up(amps=True):
    # 1. the VBC and the codec IRQ to the AP
    for a in WPROT_CP:
        setb(a, B(6))
    clrb(VBC_CTRL, 0xFFFF0 | B(18, 19))
    clrb(WPROT_AP, B(6))
    setb(APB_EB0, B(19))
    setb(PMU_SLEEP, B(8))
    clrb(PMU_MEMPD, 0x3C)
    pulse(APB_RST0, B(20))
    # 2. codec digital
    setb(APB_EB0, B(18))
    setb(VBC_CTRL, B(20))
    setb(SINDRV, B(0))
    pulse(APB_RST0, B(18, 19))
    # 3. codec analog
    adi_set(0x800, B(4))
    adi_set(0x80C, B(9, 10, 11))
    time.sleep(0.0001)
    adi_clr(0x80C, B(9, 10, 11))
    adi_set(0x644, B(2))
    adi_set(0x804, B(1, 2))
    adi_set(0x8B4, B(1))
    adi_set(0x808, B(4))
    adi_set(0x88C, B(8))
    adi_wr(0x884, 0)
    # 4. audio power: VB, BG, BG_IBIAS, VCOM (3.2 V), VCOM_BUF, VBO
    adi_set(0x640, B(7))
    adi_set(0x644, B(7))
    adi_set(0x644, B(5))
    adi_field(0x644, 11, 3, 2)
    adi_set(0x644, B(4))
    adi_set(0x644, B(3))
    adi_set(0x640, B(6))
    adi_set(0x644, B(0, 1))
    time.sleep(0.005)
    wr(AUD_SDM0, 0x400)
    # 5. the DAC at 48 kHz, unmuted
    v = rd(AUD_DAC) & ~(B(15) | 0xF)
    wr(AUD_DAC, v | 1)
    adi_set(0x660, B(0, 3))
    setb(AUD_TOP, B(0, 2))
    adi_set(0x600, B(0, 2))
    adi_wr(0x674, (adi_rd(0x674) & ~0x77C0) | B(6, 7) | (7 << 12) | (7 << 8))
    # 6. to the speaker outputs, about 0 dB
    adi_set(0x678, B(5, 0))
    adi_set(0x680, B(3, 4))
    adi_wr(0x694, (adi_rd(0x694) & ~0xFF) | 0x88)
    # 7. the VBC: buffer size, LRCK, no digital gain or patch, channels on
    wr(VBC + 0x10, (rd(VBC + 0x10) & ~0xFF00) | (159 << 8))
    setb(VBC + 0x3C, B(14))
    wr(VBC + 0x44, 0)
    wr(VBC + 0x40, 0)
    setb(VBC + 0xC8, B(0, 1))
    setb(VBC + 0x18, B(15))
    if not amps:
        return
    # 8. amplifiers last: the codec's internal PA, then the external one
    adi_field(0x64C, 13, 3, 7)
    adi_field(0x648, 11, 3, 7)
    adi_set(0x640, B(8))
    adi_wr(0x684, 0)
    # both speaker PAs: PA (left, bits 15/13) and PAR (right, bits 12/10).
    # The vendor's path only ever enables the left one; this board's speaker
    # is on the SPK-R connector (our photo), and the left PA alone
    # gave silence -- not even a switch-on pop (2026-10-28)
    adi_set(0x640, B(15, 12))
    adi_set(0x640, B(13, 10))
    gpio85(1)
    print("up")


def tone(seconds, hz=440, amp=0x2000):
    half = 48000 // hz // 2
    end = time.time() + seconds
    n = 0
    da0, da1 = _reg(VBC), _reg(VBC + 4)   # DA0 (left), DA1 (right)
    hi, lo = amp & 0xFFFF, (-amp) & 0xFFFF
    while time.time() < end:
        s = hi if (n // half) % 2 else lo
        da0.value = s
        da1.value = s
        n += 1
    print(f"tone: {n} samples in {seconds} s ({n / seconds:.0f}/s)")


def paced_tone(seconds, hz=440, amp=0x4000, rate=48000):
    """the tone at roughly the sample rate (the free-running loop above
    pushed ~150k samples/s into a 48 kHz FIFO)"""
    half = rate // hz // 2
    da0, da1 = _reg(VBC), _reg(VBC + 4)
    hi, lo = amp & 0xFFFF, (-amp) & 0xFFFF
    t0 = time.monotonic()
    n = 0
    while True:
        now = time.monotonic() - t0
        if now >= seconds:
            break
        due = int(now * rate)
        while n < due:
            s = hi if (n // half) % 2 else lo
            da0.value = s
            da1.value = s
            n += 1


def steps():
    """for our ear: each stage 3 s apart, so pops say which ran"""
    def say(m):
        print(f"{time.strftime('%T')} {m}", flush=True)
    up(amps=False)
    say("1 codec up, amplifiers off")
    time.sleep(3)
    adi_field(0x64C, 13, 3, 7)
    adi_field(0x648, 11, 3, 7)
    adi_set(0x640, B(8))
    adi_wr(0x684, 0)
    adi_set(0x640, B(15, 12))
    adi_set(0x640, B(13, 10))
    say("2 internal PAs (left and right) on")
    time.sleep(3)
    gpio85(1)
    say("3 GPIO 85 high")
    time.sleep(3)
    say("4 tone 2 s")
    paced_tone(2)
    time.sleep(3)
    gpio85(0)
    say("5 GPIO 85 low")
    time.sleep(3)
    adi_clr(0x640, B(13, 10))
    say("6 internal PAs off")
    time.sleep(1)
    down()


def allout():
    """every analog output, for an external amp of unknown input: the left
    PA alone (SPK-L) is all the internal driver has -- the right PA's bits
    (0x640 12/10) don't stick on this PMIC -- and our speaker is on
    SPK-R, next to an 8-pin amp chip GPIO 85 likely enables (2026-10-28)"""
    def say(m):
        print(f"{time.strftime('%T')} {m}", flush=True)
    up()
    adi_set(0x67C, B(15, 5, 4, 1, 0))        # DAC -> EAR, HPL, HPR
    adi_set(0x680, B(7, 6, 5))               # HPL, HPR, EAR drivers
    adi_wr(0x690, (adi_rd(0x690) & ~0xFF) | 0x88)  # HP gain, about 0 dB
    say(f"1 all outputs on (0x67C=0x{adi_rd(0x67C):04x} 0x680=0x{adi_rd(0x680):04x} 0x690=0x{adi_rd(0x690):04x}), GPIO 85 high")
    time.sleep(2)
    say("2 tone 3 s, GPIO 85 high")
    paced_tone(3)
    gpio85(0)
    say("3 GPIO 85 low")
    time.sleep(2)
    say("4 tone 3 s, GPIO 85 low")
    paced_tone(3)
    adi_clr(0x67C, B(15, 5, 4, 1, 0))
    adi_clr(0x680, B(7, 6, 5))
    down()


def down():
    gpio85(0)
    adi_clr(0x640, B(13, 10))
    time.sleep(0.002)
    adi_clr(0x640, B(15, 12, 8))
    adi_wr(0x694, adi_rd(0x694) & ~0xFF)
    adi_clr(0x678, B(5, 0))
    adi_clr(0x680, B(3, 4))
    adi_clr(0x674, B(6, 7))
    adi_clr(0x600, B(0, 2))
    clrb(AUD_TOP, B(0, 2))
    clrb(VBC + 0x18, B(15, 13, 14))
    clrb(VBC + 0xC8, B(0, 1))
    adi_clr(0x660, B(0, 3))
    adi_clr(0x640, B(6, 7))
    adi_clr(0x644, B(0, 1, 3, 4, 5, 7))
    print("down")


def dump():
    for name, a in (("APB_EB0", APB_EB0), ("VBC_CTRL", VBC_CTRL), ("AUD_TOP", AUD_TOP), ("AUD_DAC", AUD_DAC),
                    ("VBBUFFSIZE", VBC + 0x10), ("VBDABUFFDTA", VBC + 0x18), ("VBCHNEN", VBC + 0xC8)):
        print(f"{name:12s} 0x{a:08x} = 0x{rd(a):08x}")
    for off in (0x600, 0x640, 0x644, 0x648, 0x64C, 0x660, 0x674, 0x678, 0x680, 0x684, 0x694, 0x800, 0x804):
        print(f"ADI 0x{off:03x}     = 0x{adi_rd(off):04x}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "dump"
    if cmd == "up":
        up()
    elif cmd == "tone":
        tone(float(sys.argv[2]) if len(sys.argv) > 2 else 3)
    elif cmd == "allout":
        allout()
    elif cmd == "steps":
        steps()
    elif cmd == "down":
        down()
    else:
        dump()
