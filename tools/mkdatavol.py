#!/usr/bin/env python3
# SPDX-License-Identifier: MIT AND GPL-3.0-only
# Copyright (c) 2025-2026 the HakiOS authors (MIT)
# Copyright (c) 2026 Pritish Joshi (DrtSinX98) (GPL-3.0-only)
"""The data volume of Darwin for the phone: an empty HFS Plus volume in darwin past the Darwin volume and the cryptexes,
as an Android sparse image of the whole of darwin that writes that volume and nothing else.

usage: mkdatavol.py OUT.simg [MKFS]      (MKFS: mkfs.hfsplus, default the one in $PATH)

    fastboot flash darwin OUT.simg

The kernel gives that stretch of darwin out as /dev/ufs_darwin_data (drivers/storage/hakios.ufs.c, ufs_write=1) and
init mounts it on /private/var (userland/src/init.c, var_hfs=ufs_darwin_data). Flashing this again empties it.

The image says DONT_CARE for darwin's blocks before the volume -- the Darwin volume and both cryptexes, which the
bootloader therefore leaves as they are -- and, inside the volume, for every block the new volume does not use: what
is in free space does not matter to HFS Plus, and twelve gigabytes of zeros need not go over USB. The blocks it uses
(the volume headers, the allocation bitmap, the catalog and extents trees, the journal) are written whole, zeros
included: which those are is read from the volume's own allocation bitmap.

Case-sensitive (HFSX), as Darwin's own data volume is, and journaled.

    mkdatavol.py --dd OUTDIR VOLUME DARWIN_BLOCKS FIRST

The same volume for a phone without fastboot (the realme 8i: darwin 3145976 blocks, the volume from 1049088, past
the var store's slots and the kernel's breadcrumbs), from VOLUME, an image of exactly DARWIN_BLOCKS - FIRST blocks of 4096 that is already formatted
(on a Mac, newfs_hfs wants a disk: attach the image with hdiutil -nomount, newfs_hfs -s -J -b 4096 -U 0 -G 0 -M 755
-v Data on its /dev/rdiskN, detach). OUTDIR gets datavol.bin, the volume's used blocks and its last one, and
datavol.sh, which checks darwin's size and that file's SHA-256 and dd's each stretch to its place; run as root, in
Android or a recovery: sh datavol.sh [DARWIN-DEVICE]. The same two files format the volume for development and in
the installer.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

BS = 4096
DARWIN_BLOCKS = 5242875            # darwin, in blocks of 4096 (README, "The port's partition")
DATA_FIRST = 2042368               # where the volume begins: drivers/storage/hakios.ufs.c, UFS_WRITE_FIRST
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk(kind, blocks, data_size):
    return struct.pack('<HHII', kind, 0, blocks, 12 + data_size)


def used_blocks(img):
    """The volume's allocation blocks marked in use, from its header and bitmap; the block size must be BS."""
    img.seek(1024)
    vh = img.read(512)
    sig, = struct.unpack('>H', vh[0:2])
    if sig not in (0x482B, 0x4858):                     # H+, HX
        sys.exit('mkdatavol.py: no HFS Plus volume header in what mkfs made')
    block_size, total = struct.unpack('>II', vh[40:48])
    if block_size != BS:
        sys.exit(f'mkdatavol.py: the volume has blocks of {block_size}, not {BS}')
    fork = vh[112:192]                                  # allocationFile: HFSPlusForkData
    extents = [struct.unpack('>II', fork[16 + 8 * i:24 + 8 * i]) for i in range(8)]
    bitmap = bytearray()
    for start, count in extents:
        if count:
            img.seek(start * BS)
            bitmap += img.read(count * BS)
    if len(bitmap) * 8 < total:
        sys.exit('mkdatavol.py: the allocation file is in more pieces than its fork says')
    return total, lambda b: bitmap[b >> 3] & (0x80 >> (b & 7))


def main_dd(outdir, volume, darwin_blocks, first):
    """datavol.bin and datavol.sh in outdir; returns [(volume block, count)] in the order they are in datavol.bin"""
    blocks = darwin_blocks - first
    if os.path.getsize(volume) != blocks * BS:
        sys.exit(f'mkdatavol.py: {volume} is {os.path.getsize(volume)} bytes, not {blocks} blocks of {BS}')
    with open(volume, 'rb') as img:
        total, used = used_blocks(img)
        if total != blocks:
            sys.exit(f'mkdatavol.py: the volume has {total} blocks, darwin has {blocks} for it')
        runs = []
        for b in range(total):
            if used(b) or b == total - 1:               # the last block holds the alternate volume header
                if runs and runs[-1][0] + runs[-1][1] == b:
                    runs[-1][1] += 1
                else:
                    runs.append([b, 1])
        os.makedirs(outdir, exist_ok=True)
        digest = hashlib.sha256()
        with open(os.path.join(outdir, 'datavol.bin'), 'wb') as out:
            for b, count in runs:
                img.seek(b * BS)
                data = img.read(count * BS)
                out.write(data)
                digest.update(data)
    lines = ['#!/system/bin/sh',
             f'# The empty data volume into darwin (tools/mkdatavol.py --dd): its blocks {first}..{darwin_blocks - 1};',
             f'# nothing before block {first} is written. usage: sh datavol.sh [DARWIN-DEVICE]',
             'D=${1:-/dev/block/by-name/darwin}',
             'H=$(dirname "$0")',
             f'[ "$(blockdev --getsize64 "$D")" = "{darwin_blocks * BS}" ] || '
             f'{{ echo "datavol.sh: $D is not {darwin_blocks * BS} bytes: nothing written"; exit 1; }}',
             f'echo "{digest.hexdigest()}  $H/datavol.bin" | sha256sum -c - >/dev/null || '
             '{ echo "datavol.sh: datavol.bin is not what mkdatavol.py made: nothing written"; exit 1; }']
    at = 0
    for b, count in runs:
        lines.append(f'dd if="$H/datavol.bin" of="$D" bs={BS} skip={at} seek={first + b} count={count} '
                     'conv=notrunc,fsync 2>/dev/null || { echo "datavol.sh: dd failed"; exit 1; }')
        at += count
    lines.append(f'echo "datavol.sh: the data volume written: {at} blocks in {len(runs)} pieces"')
    with open(os.path.join(outdir, 'datavol.sh'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{outdir}: datavol.bin {at * BS} bytes ({at} of {blocks} blocks, {len(runs)} pieces), datavol.sh; darwin '
          f'blocks {first}..{darwin_blocks - 1}')
    return [(b, c) for b, c in runs]


def main():
    if len(sys.argv) == 6 and sys.argv[1] == '--dd':
        main_dd(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]))
        return
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = sys.argv[1]
    mkfs = sys.argv[2] if len(sys.argv) > 2 else 'mkfs.hfsplus'
    blocks = DARWIN_BLOCKS - DATA_FIRST
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(blocks * BS)
        # its root is /private/var: root's, 0755, as on an iPhone
        subprocess.run([mkfs, '-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data', path],
                       check=True)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != blocks:
                sys.exit(f'mkdatavol.py: the volume has {total} blocks, darwin has {blocks} for it')
            runs = []                                   # [kind, first, count], in order
            for b in range(total):
                kind = RAW if used(b) else DONT_CARE
                if runs and runs[-1][0] == kind and runs[-1][1] + runs[-1][2] == b:
                    runs[-1][2] += 1
                else:
                    runs.append([kind, b, 1])
            chunks, written = [chunk(DONT_CARE, DATA_FIRST, 0)], 0
            for kind, first, count in runs:
                if kind == DONT_CARE:
                    chunks.append(chunk(DONT_CARE, count, 0))
                    continue
                img.seek(first * BS)
                data = img.read(count * BS)
                # zeros in a used stretch go as FILL, which costs nothing to send; everything else as RAW
                at = 0
                while at < count:
                    zero = data[at * BS:(at + 1) * BS] == bytes(BS)
                    end = at
                    while end < count and (data[end * BS:(end + 1) * BS] == bytes(BS)) == zero:
                        end += 1
                    if zero:
                        chunks.append(chunk(FILL, end - at, 4) + bytes(4))
                    else:
                        chunks.append(chunk(RAW, end - at, (end - at) * BS) + data[at * BS:end * BS])
                    written += end - at
                    at = end
    with open(out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BS, DARWIN_BLOCKS, len(chunks), 0))
        for c in chunks:
            f.write(c)
    print(f'{out}: {os.path.getsize(out)} bytes; the volume: {blocks} blocks ({blocks * BS >> 20} MB) from darwin '
          f'block {DATA_FIRST}, {written} of them written, the rest of darwin left as it is')


if __name__ == '__main__':
    main()
