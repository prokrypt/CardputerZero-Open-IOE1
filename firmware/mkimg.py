#!/usr/bin/env python3
"""mkimg.py IN.bin OUT.bin: pad an app to the 20 KB slot (0xFF) and put CRC-32/MPEG-2 (byte-wise, init
0xFFFFFFFF, no xor) of the first 0x4FFC bytes little-endian in the last 4, the trailer M5's bootloader checks."""
import struct
import sys

SIZE = 0x5000


def crc_mpeg(d, c=0xffffffff):
    for x in d:
        c ^= x << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    assert len(d) <= SIZE - 4, ('image too big', len(d))
    d += b'\xff' * (SIZE - 4 - len(d))
    open(sys.argv[2], 'wb').write(d + struct.pack('<I', crc_mpeg(d)))
    print(sys.argv[2], len(d) + 4, 'bytes, code', len(open(sys.argv[1], 'rb').read()))
