#!/usr/bin/env python3
# a FAT16 card image for host/fs_test: the files the list must show and the ones it must skip
import os, sys
from pyfatfs.PyFatFS import PyFat, PyFatFS

path = sys.argv[1] if len(sys.argv) > 1 else "test.img"
if os.path.exists(path):
    os.unlink(path)
with open(path, 'wb') as f:
    f.truncate(16 * 1024 * 1024)
fat = PyFat()
fat.mkfs(path, PyFat.FAT_TYPE_FAT16, size=16 * 1024 * 1024, label="PLAYER")
fat.close()
fs = PyFatFS(path)
fs.makedirs("/sub")
fs.writebytes("/a.mod", bytes((i * 7) & 255 for i in range(5000)))
fs.writebytes("/b.XM", b"xm" * 100)
fs.writebytes("/skip.txt", b"no")
fs.writebytes("/.hidden.mod", b"hidden")
fs.writebytes("/sub/c.s3m", b"s3m" * 10)
fs.makedirs("/many")
for i in range(600):
    fs.writebytes("/many/f%04d.mod" % i, b"")
fs.close()
print(path, os.path.getsize(path))
