#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Pritish Joshi (DrtSinX98)
"""
Dry runs of mapple-install's split- verbs against a file the size of the phone's unit, with its GPT copies:

  split-test.py MAPPLE_INSTALL STOCK_HEAD STOCK_TAIL [HEAD TAIL ...] [--boot ANDROID_BOOT]

STOCK_* are the table of a phone without xnusys (gpt-head.bin / gpt-tail.bin as read off it); any further pairs
are tables carved earlier by hand: split-carve must refuse them, and split-uninstall must give back the stock table
byte for byte when xnusys alone follows userdata, or refuse and leave the table when darwin is there too. The boot image is made up (a v2 header, a gzipped
stand-in Image) unless --boot gives a copy of a real one. Every write is also failed once (MAPPLE_FAIL_AT) to
check that what was written is put back. Sparse files in a temporary directory; nothing given is changed.
"""
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

BS = 4096
MIB = 1 << 20
AREAS = 128 * MIB


def gpt_parts(h):
    """name -> (index, first, last) from a primary copy's bytes"""
    ent, n, esz = struct.unpack_from('<QII', h, BS + 72)
    e = h[ent * BS:ent * BS + n * esz]
    return {e[i * esz + 56:(i + 1) * esz].decode('utf-16-le').rstrip('\0'): (i, *struct.unpack_from('<QQ', e, i * esz + 32))
            for i in range(n) if e[i * esz:i * esz + 16] != bytes(16)}


def rd(path, off, n):
    with open(path, 'rb') as f:
        f.seek(off)
        return f.read(n)


def wr(path, off, data):
    with open(path, 'r+b') as f:
        f.seek(off)
        f.write(data)


def sha(b):
    return hashlib.sha256(b).digest()


def fake_boot(size, seed):
    """an Android v2 boot image of `size` bytes: gzipped stand-in Image, ramdisk, dtb"""
    page = 2048
    image = bytearray(os.urandom(0x40) + seed * 3000 + os.urandom(200000))
    image[0x38:0x3c] = b'ARM\x64'
    struct.pack_into('<Q', image, 0x10, 0x2000000)
    struct.pack_into('<Q', image, 0x28, 0)
    kernel = gzip.compress(bytes(image), 9, mtime=0)
    ramdisk, dtb = os.urandom(5000), os.urandom(3000)
    hdr = bytearray(page)
    hdr[0:8] = b'ANDROID!'
    struct.pack_into('<9I', hdr, 8, len(kernel), 0x40008000, len(ramdisk), 0x41000000, 0, 0, 0x40000100, page, 2)
    struct.pack_into('<I', hdr, 1648, len(dtb))
    pad = lambda b: b + bytes(-len(b) % page)
    img = bytes(hdr) + pad(kernel) + pad(ramdisk) + pad(dtb)
    return img + bytes(size - len(img))


def chained(img):
    page, ksize = struct.unpack_from('<I', img, 36)[0], struct.unpack_from('<I', img, 8)[0]
    k = img[page:page + ksize]
    return k[:2] == b'\x1f\x8b' and struct.unpack_from('<Q', gzip.decompress(k), 0x28)[0] != 0


class Phone:
    def __init__(self, d, head, tail, boot, tool, loader, payload):
        os.makedirs(d)
        self.disk, self.boot, self.tool = os.path.join(d, 'disk'), os.path.join(d, 'boot'), tool
        self.loader, self.payload = loader, payload
        hd, tl = open(head, 'rb').read(), open(tail, 'rb').read()
        self.blocks = struct.unpack_from('<Q', hd, BS + 32)[0] + 1
        with open(self.disk, 'wb') as f:
            f.truncate(self.blocks * BS)
        wr(self.disk, 0, hd)
        wr(self.disk, self.blocks * BS - len(tl), tl)
        p = self.parts()
        self.boot_off, self.boot_n = p['boot'][1] * BS, (p['boot'][2] - p['boot'][1] + 1) * BS
        open(self.boot, 'wb').close()
        self.set_boot(boot if boot else fake_boot(self.boot_n, b'A'))
        for name, n in (('metadata', MIB), ('userdata', 64 * MIB)):
            wr(self.disk, p[name][1] * BS, b'\xa5' * n)

    def parts(self):
        return gpt_parts(rd(self.disk, 0, 64 * BS))

    def set_boot(self, img):
        """BOOT and DISK's boot extent are one partition on the phone"""
        img = img + bytes(self.boot_n - len(img))
        wr(self.boot, 0, img)
        wr(self.disk, self.boot_off, img)

    def boot_img(self):
        return open(self.boot, 'rb').read()

    def tables(self):
        return rd(self.disk, 0, 8 * BS), rd(self.disk, self.blocks * BS - 8 * BS, 8 * BS)

    def snap(self):
        return self.tables(), sha(self.boot_img())

    def verb(self, v, *extra, fail=None, ok=True):
        a = {'carve': ['split-carve', self.disk],
             'chain': ['split-chain', self.disk, self.boot, self.loader, self.payload],
             'uninstall': ['split-uninstall', self.disk, self.boot]}[v] + list(extra)
        env = {k: x for k, x in os.environ.items() if k != 'MAPPLE_FAIL_AT'}
        if fail:
            env['MAPPLE_FAIL_AT'] = str(fail)
        p = subprocess.run([self.tool] + a, capture_output=True, text=True, env=env)
        out = p.stdout + p.stderr
        if ok is not None and (p.returncode == 0) != ok:
            sys.exit(f'FAIL: {v} {extra} (fail at {fail}) exited {p.returncode}\n{out}')
        self.set_boot(self.boot_img())
        return p.returncode == 0, out

    def fail_each(self, v, expect):
        """the verb failed at each of its writes in turn: the GPT and boot as they were; then once to the end"""
        before = self.snap()
        n = 0
        while True:
            n += 1
            done, out = self.verb(v, fail=n, ok=None)
            if done:
                break
            assert self.snap() == before, f'{v} failing at write {n}: not put back\n{out}'
        assert n - 1 == expect, f'{v}: {n - 1} checked writes, expected {expect}\n{out}'

    def saved(self):
        """the saved area's items: name -> (size, sha256)"""
        x = self.parts()['xnusys']
        h = rd(self.disk, (x[2] + 1) * BS - AREAS, 4096)
        assert h[:16] == b'mapple-saved\0\0\0\0' and sha(h[:4064]) == h[4064:], 'saved header'
        return {h[64 + 64 * i:80 + 64 * i].rstrip(b'\0').decode(): (struct.unpack_from('<Q', h, 80 + 64 * i)[0],
                                                                 h[96 + 64 * i:128 + 64 * i])
                for i in range(struct.unpack_from('<I', h, 20)[0])}


def main():
    args, boot = sys.argv[1:], None
    if '--boot' in args:
        i = args.index('--boot')
        boot = open(args[i + 1], 'rb').read()
        del args[i:i + 2]
    tool, pairs = os.path.abspath(args[0]), list(zip(args[1::2], args[2::2]))
    d = tempfile.mkdtemp(prefix='split-test-')
    loader, payload = os.path.join(d, 'loader.bin'), os.path.join(d, 'xnup.bin')
    prog = bytearray(4096)
    prog[0x38:0x3c] = b'ARM\x64'
    open(loader, 'wb').write(prog)
    open(payload, 'wb').write(os.urandom(300000))
    ph = Phone(os.path.join(d, 'stock'), *pairs[0], boot, tool, loader, payload)
    stock_tables, stock_boot, stock_ud = ph.tables(), ph.boot_img(), ph.parts()['userdata']
    checks = 0

    ph.verb('carve', '--check')
    assert ph.tables() == stock_tables and ph.boot_img() == stock_boot, 'carve --check wrote something'
    ph.fail_each('carve', 6)
    p = ph.parts()
    u, x, m = p['userdata'], p['xnusys'], p['metadata']
    assert u[1] == stock_ud[1] and u[2] + 1 == x[1] and x[2] == stock_ud[2] and x[1] % 256 == 0
    assert 22 << 30 <= (x[2] - x[1] + 1) * BS < (22 << 30) + MIB, 'xnusys is not 22 GiB (and the alignment\'s rest)'
    for off, n in ((x[1] * BS, MIB), ((x[2] + 1) * BS - AREAS, MIB), (u[1] * BS, 64 * MIB),
                   (m[1] * BS, (m[2] - m[1] + 1) * BS)):
        assert rd(ph.disk, off, n) == bytes(n), f'not zeroed at {off}'
    assert ph.boot_img() == stock_boot, 'carve touched boot'
    ph.verb('carve', ok=False)
    checks += 11
    print(f'carve: put back at each of its 6 writes; then xnusys {x[1]}..{x[2]} at userdata\'s end, '
          f'zeroed as planned, boot untouched; refused once carved')

    ph.fail_each('chain', 4)
    chained1 = ph.boot_img()
    it = ph.saved()
    assert chained(chained1) and it['boot'] == (len(stock_boot), sha(stock_boot)) and it['chained'][1] == sha(chained1)
    assert rd(ph.disk, (x[2] + 1) * BS - AREAS + MIB, len(stock_boot)) == stock_boot
    ph.verb('chain')
    assert ph.boot_img() == chained1 and ph.saved()['boot'][1] == sha(stock_boot)
    ota = fake_boot(ph.boot_n, b'B')
    ph.set_boot(ota)
    ph.verb('chain')
    now = ph.boot_img()
    assert chained(now) and ph.saved()['boot'][1] == sha(ota) and ph.saved()['chained'][1] == sha(now)
    checks += 8
    print('chain: put back at each of its 4 writes; stock boot saved, chained, recorded; chained again: '
          'unchanged; an update\'s boot saved anew and chained')

    ph.verb('uninstall', '--check')
    ph.fail_each('uninstall', 5)
    assert ph.tables() == stock_tables and ph.boot_img() == ota
    ph.verb('uninstall', ok=False)
    checks += 7
    print('uninstall: put back at each of its 5 writes; then the stock GPT byte for byte, boot = the update\'s')

    ph.verb('carve')
    ph.verb('chain')
    ph.verb('uninstall')
    assert ph.tables() == stock_tables and ph.boot_img() == ota
    ph.verb('carve')
    ph.verb('chain')
    other = fake_boot(ph.boot_n, b'C')
    other_img = os.path.join(d, 'other.img')
    open(other_img, 'wb').write(other)
    subprocess.run([tool, 'chain', other_img, loader, payload], check=True, capture_output=True)
    ph.set_boot(open(other_img, 'rb').read())
    before = ph.snap()
    _, out = ph.verb('uninstall', ok=False)
    assert ph.snap() == before and 'not the image chained here' in out
    ph.set_boot(other)
    _, out = ph.verb('uninstall')
    assert ph.tables() == stock_tables and ph.boot_img() == other and 'left as it is' in out
    checks += 5
    print('carve, chain, uninstall: back to stock; a boot with a loader not chained here: refused; '
          'a plain one: left, GPT stock')

    for k, (head, tail) in enumerate(pairs[1:]):
        q = Phone(os.path.join(d, f'table{k}'), head, tail, None, tool, loader, payload)
        before = q.snap()
        q.verb('carve', ok=False)
        assert q.snap() == before
        if 'darwin' in q.parts():
            q.verb('uninstall', ok=False)
            assert q.snap() == before
            print(f'{os.path.basename(head)}: carve and uninstall refused, nothing written')
        else:
            q.verb('uninstall')
            assert q.tables() == stock_tables, f'{head}: uninstall did not give the stock table'
            print(f'{os.path.basename(head)}: carve refused; uninstall gives the stock table byte for byte')
        checks += 2
    shutil.rmtree(d)
    print(f'split-test: {checks} checks passed')


if __name__ == '__main__':
    main()
