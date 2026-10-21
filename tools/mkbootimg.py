#!/usr/bin/env python3
"""Minimal Android boot image writer, header v0-v2 (the subset shim/build.sh uses).

Takes the same options as AOSP mkbootimg so build.sh can use either:
  mkbootimg.py --header_version N --kernel K [--ramdisk R] [--dtb D] [--base B]
               [--kernel_offset ..] [--ramdisk_offset ..] [--tags_offset ..]
               [--dtb_offset ..] [--pagesize P] [--os_version A.B.C]
               [--os_patch_level YYYY-MM] [--cmdline S] -o OUT
"""
import argparse, hashlib, struct

p = argparse.ArgumentParser()
num = lambda s: int(s, 0)
p.add_argument('--header_version', type=int, default=0)
p.add_argument('--kernel', required=True)
p.add_argument('--ramdisk')
p.add_argument('--dtb')
p.add_argument('--base', type=num, default=0x10000000)
p.add_argument('--kernel_offset', type=num, default=0x8000)
p.add_argument('--ramdisk_offset', type=num, default=0x1000000)
p.add_argument('--second_offset', type=num, default=0xf00000)
p.add_argument('--tags_offset', type=num, default=0x100)
p.add_argument('--dtb_offset', type=num, default=0x1f00000)
p.add_argument('--pagesize', type=num, default=2048)
p.add_argument('--os_version', default='0.0.0')
p.add_argument('--os_patch_level', default='2000-01')
p.add_argument('--cmdline', default='')
p.add_argument('--board', default='')
p.add_argument('-o', '--output', required=True)
a = p.parse_args()
assert a.header_version in (0, 1, 2), 'only header v0-v2'
assert a.header_version == 2 or not a.dtb, '--dtb needs header v2'

read = lambda f: open(f, 'rb').read() if f else b''
kernel, ramdisk, dtb = read(a.kernel), read(a.ramdisk), read(a.dtb)

maj, mnr, pat = (int(x) for x in a.os_version.split('.'))
y, m = (int(x) for x in a.os_patch_level.split('-')[:2])
os_version = (maj << 25 | mnr << 18 | pat << 11) | ((y - 2000) << 4 | m)

cmd = a.cmdline.encode()
assert len(cmd) < 512 + 1024, 'cmdline too long'

# id: SHA-1 over each section followed by its size, as AOSP mkbootimg
sha = hashlib.sha1()
sections = [kernel, ramdisk, b'']	# second stage unused
if a.header_version >= 1:
    sections.append(b'')		# recovery dtbo unused
if a.header_version >= 2:
    sections.append(dtb)
for s in sections:
    sha.update(s)
    sha.update(struct.pack('<I', len(s)))

hdr = bytearray(a.pagesize)
struct.pack_into('<8s10I', hdr, 0, b'ANDROID!',
                 len(kernel), a.base + a.kernel_offset,
                 len(ramdisk), a.base + a.ramdisk_offset,
                 0, a.base + a.second_offset,
                 a.base + a.tags_offset, a.pagesize, a.header_version, os_version)
struct.pack_into('16s', hdr, 48, a.board.encode())
struct.pack_into('512s', hdr, 64, cmd[:511])
struct.pack_into('32s', hdr, 576, sha.digest())
struct.pack_into('1024s', hdr, 608, cmd[511:])
if a.header_version >= 1:
    struct.pack_into('<IQI', hdr, 1632, 0, 0, 1648 if a.header_version == 1 else 1660)
if a.header_version >= 2:
    struct.pack_into('<IQ', hdr, 1648, len(dtb), a.base + a.dtb_offset)

pad = lambda b: b + b'\0' * (-len(b) % a.pagesize)
with open(a.output, 'wb') as f:
    f.write(bytes(hdr))
    for s in (kernel, ramdisk, dtb):
        f.write(pad(s))
