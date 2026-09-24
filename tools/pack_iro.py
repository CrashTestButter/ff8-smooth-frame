#!/usr/bin/env python3
"""Pack a folder into a Junction VIII .iro archive (no compression).

Writes the same layout as J8's IrosArc.Create with CompressType.Nothing and ArchiveFlags.None
(AppWrapper/IrosArc.cs, Junction-VIII, MS-PL):
  header : int32 'IROS' (0x534F5249), int32 version 0x10002, int32 flags 0, int32 directory offset 16
  dir    : int32 count, then per file: u16 entry_len, u16 name_bytes, UTF-16LE name ('\\' separators),
           int32 file flags (0 = stored), int64 offset, int32 length
  data   : raw file bytes
All little endian. Usage: pack_iro.py <folder> <out.iro>  (then it re-reads and verifies the archive)
"""
import os, struct, sys

SIG, VERSION = 0x534F5249, 0x10002


def pack(src, out):
    names = sorted(os.path.relpath(os.path.join(r, f), src).replace('/', '\\')
                   for r, _, fs in os.walk(src) for f in fs)
    if not names:
        sys.exit('nothing to pack')
    blobs = [open(os.path.join(src, n.replace('\\', os.sep)), 'rb').read() for n in names]
    enc = [n.encode('utf-16-le') for n in names]
    dsize = sum(len(e) + 4 + 16 for e in enc)
    pos = 16 + 4 + dsize
    head = struct.pack('<iiii', SIG, VERSION, 0, 16) + struct.pack('<i', len(names))
    dirent, data = b'', b''
    for e, b in zip(enc, blobs):
        dirent += struct.pack('<HH', len(e) + 4 + 16, len(e)) + e + struct.pack('<iqi', 0, pos, len(b))
        pos += len(b)
        data += b
    with open(out, 'wb') as f:
        f.write(head + dirent + data)
    return dict(zip(names, blobs))


def read(path):
    d = open(path, 'rb').read()
    sig, ver, flags, dirofs = struct.unpack_from('<iiii', d, 0)
    assert sig == SIG and 0x10000 <= ver <= 0x10002, 'bad header'
    n, = struct.unpack_from('<i', d, dirofs)
    p, files = dirofs + 4, {}
    for _ in range(n):
        ln, fl = struct.unpack_from('<HH', d, p)
        name = d[p + 4:p + 4 + fl].decode('utf-16-le')
        ff, off, length = struct.unpack_from('<iqi', d, p + 4 + fl)
        assert ff == 0 and off + length <= len(d)
        files[name] = d[off:off + length]
        p += ln
    return files


if __name__ == '__main__':
    src, out = sys.argv[1], sys.argv[2]
    want = pack(src, out)
    got = read(out)
    assert got == want, 'verify failed'
    print(f'{out}: {len(got)} files, {os.path.getsize(out)} bytes, verified: ' + ', '.join(sorted(got)))
