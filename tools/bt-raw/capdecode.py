# capdecode.py K HEX [CH] -- dewhiten an RX-buffer capture taken with the
# correlator window at air bit K (0 = first access-address bit): the capture
# starts after the 64 sync bits, the 4-bit trailer, the 54-bit BR header and a
# 16-bit DH3 payload header, i.e. at air bit K+138; print the plain PDU bytes
# from the next byte boundary, with their PDU offsets
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from lepattern import whiten_bits_spec
k = int(sys.argv[1]); cap = bytes.fromhex(sys.argv[2]); ch = int(sys.argv[3]) if len(sys.argv) > 3 else 37
bits = [(x >> i) & 1 for x in cap for i in range(8)]
pdu_bit = k + 64 + 4 + 54 + 16 - 32
w = whiten_bits_spec(ch, pdu_bit + len(bits))
plain = [b ^ w[pdu_bit + i] for i, b in enumerate(bits)]
skip = (8 - pdu_bit % 8) % 8
by = bytes(sum(plain[skip + 8 * i + j] << j for j in range(8)) for i in range((len(plain) - skip) // 8))
print('PDU bytes from %d: %s %r' % ((pdu_bit + skip) // 8, by.hex(), by))
