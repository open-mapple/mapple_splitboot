#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Pritish Joshi (DrtSinX98)
"""mkdatavol.py --dd on a made-up HFS Plus volume of 16 blocks: datavol.sh's dd lines, applied to a made-up darwin,
must write the used blocks and the last one where they belong and nothing else.  usage: mkdatavol-test.py"""
import importlib.util
import os
import random
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('mkdatavol', os.path.join(HERE, 'mkdatavol.py'))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)
BS, TOTAL, FIRST, DARWIN = mk.BS, 16, 5, 21
USED = {0, 1, 2, 7}                             # the header, the bitmap, two more; 15 (the last) is added by the tool

rnd = random.Random(1)
vol = bytearray(rnd.randbytes(TOTAL * BS))      # what is in free space must not be written
vh = bytearray(512)
vh[0:2] = b'H+'
vh[40:48] = struct.pack('>II', BS, TOTAL)
vh[112 + 16:112 + 24] = struct.pack('>II', 1, 1)  # the allocation file: block 1, one block
vol[1024:1536] = vh
bitmap = bytearray(BS)
for b in USED:
    bitmap[b >> 3] |= 0x80 >> (b & 7)
vol[BS:2 * BS] = bitmap

with tempfile.TemporaryDirectory() as tmp:
    vpath = os.path.join(tmp, 'vol.img')
    open(vpath, 'wb').write(vol)
    runs = mk.main_dd(os.path.join(tmp, 'out'), vpath, DARWIN, FIRST)
    assert runs == [(0, 3), (7, 1), (15, 1)], runs
    darwin = bytearray(b'\xee' * DARWIN * BS)
    before = bytes(darwin)
    blob = open(os.path.join(tmp, 'out', 'datavol.bin'), 'rb').read()
    script = open(os.path.join(tmp, 'out', 'datavol.sh')).read()
    assert f'= "{DARWIN * BS}" ]' in script
    for skip, seek, count in re.findall(r'bs=4096 skip=(\d+) seek=(\d+) count=(\d+)', script):
        skip, seek, count = int(skip), int(seek), int(count)
        assert seek >= FIRST
        darwin[seek * BS:(seek + count) * BS] = blob[skip * BS:(skip + count) * BS]
    for b in range(DARWIN):
        got = darwin[b * BS:(b + 1) * BS]
        v = b - FIRST
        if v >= 0 and (v in USED or v == TOTAL - 1):
            assert got == vol[v * BS:(v + 1) * BS], f'darwin block {b}: not the volume\'s block {v}'
        else:
            assert got == before[b * BS:(b + 1) * BS], f'darwin block {b}: written, and should not be'
    # the read-back lines read exactly what was written, and their SHA-256 is the one the script expects
    import hashlib
    back = b''.join(bytes(darwin[int(sk) * BS:(int(sk) + int(c)) * BS])
                    for sk, c in re.findall(r'dd if="\$D" bs=4096 skip=(\d+) count=(\d+)', script))
    want = re.search(r'\[ "\$R" = "([0-9a-f]{64})" \]', script).group(1)
    assert hashlib.sha256(back).hexdigest() == want == hashlib.sha256(blob).hexdigest()

    # a volume smaller than the room after FIRST, into another partition: nothing after it is written
    runs = mk.main_dd(os.path.join(tmp, 'small'), vpath, DARWIN + 7, FIRST, 'xnusys')
    script = open(os.path.join(tmp, 'small', 'datavol.sh')).read()
    assert 'by-name/xnusys' in script and f'= "{(DARWIN + 7) * BS}" ]' in script
    assert max(int(sk) + int(c) for _, sk, c in re.findall(r'bs=4096 skip=(\d+) seek=(\d+) count=(\d+)', script)) == FIRST + TOTAL
print('mkdatavol-test.py: ok')
sys.exit(0)
