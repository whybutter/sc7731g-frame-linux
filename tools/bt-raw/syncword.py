# syncword.py -- the firmware's own BR sync-word routine (wcnmodem 0x8a836fc8),
# replayed instruction for instruction: syncword(lap) -> (word_r2, word_r1)
M = 0xffffffff
K24, K28, K2C, K30 = 0xb8b53f7b, 0x4c3e0f5c, 0x1cf7fbe1, 0x83848d96   # filled in below


def load_consts(fw):
    import struct
    base = 0x8a808000
    global K24, K28, K2C, K30
    K24, K28, K2C, K30 = struct.unpack_from('<4I', fw, 0x8a837024 - base)


def s32(x):
    return x - (1 << 32) if x & 0x80000000 else x


def syncword(lap):
    r0 = lap
    r3 = (r0 << 8) & M
    r0 |= (19 << 24) if s32(r3) < 0 else (11 << 26)
    r1 = K2C
    r0 ^= r1
    r4 = (r0 << 2) & M
    r0 = 0
    r3 = 1
    while True:
        if s32(r4) < 0:
            r4 ^= K24
            r0 ^= 1 << 29
        r7 = r0 >> 31
        r4 = ((r4 << 1) + r7) & M
        r0 = (r0 << 1) & M
        r3 += 1
        if r3 > 30:
            break
    out_r2 = ((r4 << 2) & M) ^ (r0 >> 30) ^ K28
    out_r1 = (r4 >> 30) ^ r1 ^ K30
    return out_r2, out_r1


if __name__ == '__main__':
    import sys
    load_consts(open(sys.argv[1], 'rb').read())
    for lap in sys.argv[2:]:
        a, b = syncword(int(lap, 16))
        print('lap %s -> %08x %08x' % (lap, a, b))
