# lepattern.py -- the 64 on-air bits after a BLE preamble (AA + whitened PDU
# header + whitened first 2 bytes of AdvA), packed the way the VT10101's BR
# correlator holds them (0x40086008 bit 0 = first bit on air, then 0x4008600c)
AA = 0x8E89BED6


def whiten_bits_spec(ch, n):
    """Core spec Vol 6 Part B 3.2: 7-bit LFSR x^7+x^4+1, position 0 = 1,
    positions 1..6 = channel index MSB..LSB; output = position 6"""
    pos = [1] + [(ch >> (5 - i)) & 1 for i in range(6)]
    out = []
    for _ in range(n):
        o = pos[6]
        out.append(o)
        new = [o] + pos[:6]
        new[4] = pos[3] ^ o
        pos = new
    return out


def whiten_bits_grinberg(ch, n):
    """Dmitry Grinberg's BTLEWhiten (coefficient = 8-bit-reversed channel | 2,
    output bit 7, feedback ^0x11), known to work on real radios: the
    independent check on the spec form above. (A "reversed(ch) << 1 | 1,
    ^0x88" variant I tried first disagreed with both and was wrong.)"""
    c = int('{:08b}'.format(ch)[::-1], 2) | 2
    out = []
    for _ in range(n):
        o = (c >> 7) & 1
        out.append(o)
        if o:
            c ^= 0x11
        c = (c << 1) & 0xff
    return out


def bits_lsb(data):
    return [(b >> i) & 1 for b in data for i in range(8)]


def pattern(pdu_first4, ch, whiten=whiten_bits_spec):
    aa = [(AA >> i) & 1 for i in range(32)]
    w = whiten(ch, 32)
    pdu = [b ^ x for b, x in zip(bits_lsb(pdu_first4), w)]
    s = aa + pdu
    word = sum(b << i for i, b in enumerate(s))
    return word & 0xffffffff, word >> 32


if __name__ == '__main__':
    import sys
    assert all(whiten_bits_spec(c, 64) == whiten_bits_grinberg(c, 64) for c in (37, 38, 39)), 'whitening forms disagree'
    hdr0, ln = int(sys.argv[1], 0), int(sys.argv[2], 0)
    adva = bytes(int(x, 16) for x in reversed(sys.argv[3].split(':')))
    ch = int(sys.argv[4]) if len(sys.argv) > 4 else 37
    lo, hi = pattern(bytes([hdr0, ln]) + adva[:2], ch)
    print('ch%d hdr %02x len %d %s -> lo %08x hi %08x' % (ch, hdr0, ln, sys.argv[3], lo, hi))
