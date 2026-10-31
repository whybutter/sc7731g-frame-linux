# brview.py -- what the VT10101's BR receiver makes of a BLE packet.
# Builds a legacy advert's on-air bits (preamble, AA, whitened PDU + CRC24),
# then, for a correlation window starting at bit k, the 4-bit trailer and the
# 54-bit BR header the baseband decodes after the 64 sync bits: majority of
# each bit triple (FEC 1/3) -> LT_ADDR(3) TYPE(4) FLOW ARQN SEQN HEC(8), LSB
# first, and the UAP that would make that HEC check pass.
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from lepattern import whiten_bits_spec, bits_lsb, AA


def crc24(pdu):
    """BLE CRC: poly x^24+x^10+x^9+x^6+x^4+x^3+x+1, init 0x555555, data LSB
    first; the 24 bits go on air MSB of the register first (Grinberg's form)"""
    st = 0x555555
    for bit in bits_lsb(pdu):
        fb = ((st >> 23) & 1) ^ bit
        st = (st << 1) & 0xffffff
        if fb:
            st ^= 0x00065b
    return [(st >> (23 - i)) & 1 for i in range(24)]


def air_bits(pdu, ch=37):
    pre = [0, 1] * 4 if AA & 1 else [1, 0] * 4   # preamble ends opposite to AA bit 0
    aa = [(AA >> i) & 1 for i in range(32)]
    body = bits_lsb(pdu) + crc24(pdu)
    w = whiten_bits_spec(ch, len(body))
    return pre, aa + [b ^ x for b, x in zip(body, w)]


def hec(bits10, uap):
    """BR HEC: 8-bit LFSR D^8+D^7+D^5+D^2+D+1, register initialised with the
    UAP (bit-reversed into the register, as in the spec's figure), header
    bits LSB first; returns the 8 HEC bits in transmit order"""
    r = [(uap >> (7 - i)) & 1 for i in range(8)]    # r[0]..r[7]
    for b in bits10:
        fb = b ^ r[7]
        r = [fb, r[0] ^ fb, r[1] ^ fb, r[2], r[3], r[4] ^ fb, r[5], r[6] ^ fb]
    return r[::-1]


def br_header(bits, k):
    hdr = bits[k + 64 + 4:k + 64 + 4 + 54]
    if len(hdr) < 54:
        return None
    dec = [1 if sum(hdr[3 * i:3 * i + 3]) >= 2 else 0 for i in range(18)]
    errs = sum(1 for i in range(18) if len(set(hdr[3 * i:3 * i + 3])) > 1)
    h10, hec_rx = dec[:10], dec[10:]
    # the hardware takes the UAP the other way round from this hec(): the
    # sweep (bt6) found the C3's k=7 header passing at 0xD2, the bit reversal
    # of the 0x4B computed here. 'uaps' is what to write into the link.
    uaps = [int('{:08b}'.format(u)[::-1], 2) for u in range(256) if hec(h10, u) == hec_rx]
    lt = h10[0] | h10[1] << 1 | h10[2] << 2
    typ = h10[3] | h10[4] << 1 | h10[5] << 2 | h10[6] << 3
    return dict(lt=lt, type=typ, flow=h10[7], arqn=h10[8], seqn=h10[9], fec_disagree=errs, uaps=uaps)


if __name__ == '__main__':
    # the C3 beacon: ADV_NONCONN, TxAdd random, AdvA C3:BE:12:00:00:01, flags + mfg 'BERYL'
    adva = bytes(reversed(bytes.fromhex('c3be12000001')))
    payload = adva + bytes.fromhex('020106') + bytes([8, 0xff, 0xff, 0xff]) + b'BERYL'
    pdu = bytes([0x42, len(payload)]) + payload
    pre, bits = air_bits(pdu)
    lo = sum(b << i for i, b in enumerate(bits[:32])); hi = sum(b << i for i, b in enumerate(bits[32:64]))
    print('pdu', pdu.hex(), 'bits', len(bits), 'window k=0: %08x %08x' % (lo, hi))
    for k in range(0, 40):
        h = br_header(bits, k)
        if h:
            w = bits[k:k + 64]
            lo = sum(b << i for i, b in enumerate(w[:32])); hi = sum(b << i for i, b in enumerate(w[32:]))
            print('k=%2d sync %08x %08x  lt=%d type=%2d fec_disagree=%2d uap=%s' % (
                k, lo, hi, h['lt'], h['type'], h['fec_disagree'], h['uaps']))
