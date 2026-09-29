#!/usr/bin/env python3
# usage: make-noscav.py <jsc>   writes <jsc>-noscav: a copy with the initial value of pas_scavenger_is_enabled set to 0.
import subprocess, sys, shutil, os
src = sys.argv[1]; dst = src + '-noscav'
addr = None
for line in subprocess.run(['nm', src], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[2] == 'pas_scavenger_is_enabled':
        addr = int(p[0], 16)
assert addr is not None
off = None
for line in subprocess.run(['readelf', '-S', '-W', src], capture_output=True, text=True).stdout.splitlines():
    p = line.replace('[', ' ').replace(']', ' ').split()
    if len(p) >= 6 and p[1] == '.data':
        a, o, s = int(p[3], 16), int(p[4], 16), int(p[5], 16)
        assert a <= addr < a + s
        off = o + (addr - a)
assert off is not None
shutil.copyfile(src, dst); os.chmod(dst, 0o755)
with open(dst, 'r+b') as f:
    f.seek(off); v = f.read(1); assert v == b'\x01', v
    f.seek(off); f.write(b'\x00')
print(dst, hex(addr), hex(off))
