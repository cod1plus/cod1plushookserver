#!/usr/bin/env python3
"""make_hdtex_fix.py - build zcod9_fix.pk3 from the HD texture pack's own pk3s.

    python3 scripts/make_hdtex_fix.py <dir with zcod1..6.pk3> [-o zcod9_fix.pk3]

The 1.1 engine reads a .dds only when its FourCC is DXT1, DXT3 or DXT5 (CoDMP 0x50d365).
17 textures of the pack were saved with the DX10 extended header (FourCC "DX10" + 20
bytes, dxgi format 76..78 = BC3): the engine rejects them, and since they shadow the
vanilla .dds of the same name those decals are gone (default shader on the map).
The pixel data is plain BC3 = DXT5: this writes the same files with a legacy header
(FourCC DXT5, the 20 extra bytes dropped) into a pak that sorts after zcod1..6.

Also reports what it cannot fix (non power-of-two DDS: the engine falls back to the
vanilla .tga / .jpg when there is one).

The pak's checksum only depends on the CRC-32 of its files, not on how they are
deflated: after a rebuild, scripts/gen_pure_extra.py gives the same number.
"""
import argparse
import glob
import os
import struct
import sys
import zipfile

FOURCC = {70: b"DXT1", 71: b"DXT1", 72: b"DXT1", 73: b"DXT3", 74: b"DXT3", 75: b"DXT3",
          76: b"DXT5", 77: b"DXT5", 78: b"DXT5"}


def mip_bytes(w, h, mips, block):
    total = 0
    for m in range(max(1, mips)):
        total += max(1, (w >> m) // 4) * max(1, (h >> m) // 4) * block
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("-o", "--out", default="zcod9_fix.pk3")
    a = ap.parse_args()

    paks = sorted(p for p in glob.glob(os.path.join(a.dir, "zcod*.pk3"))
                  if os.path.abspath(p) != os.path.abspath(a.out) and "fix" not in os.path.basename(p))
    if not paks:
        print("no zcod*.pk3 in %s" % a.dir, file=sys.stderr)
        return 1
    done = 0
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as out:
        for p in paks:
            with zipfile.ZipFile(p) as z:
                for i in z.infolist():
                    if i.file_size < 148 or not i.filename.lower().endswith(".dds"):
                        continue
                    with z.open(i) as f:
                        h = f.read(148)
                    if h[:4] != b"DDS ":
                        continue
                    height, width = struct.unpack_from("<II", h, 12)
                    mips = struct.unpack_from("<I", h, 28)[0]
                    if h[84:88] != b"DX10":
                        if width & (width - 1) or height & (height - 1):
                            print("not fixable (%dx%d, not a power of two): %s" % (width, height, i.filename))
                        continue
                    dxgi, dim, _misc, arr, _misc2 = struct.unpack_from("<5I", h, 128)
                    four = FOURCC.get(dxgi)
                    data = z.read(i)
                    body = data[148:]
                    if four is None or dim != 3 or arr != 1 or \
                       len(body) != mip_bytes(width, height, mips, 8 if four == b"DXT1" else 16):
                        print("not fixable (dxgi %d, %d bytes): %s" % (dxgi, len(body), i.filename))
                        continue
                    hdr = bytearray(data[:128])
                    hdr[84:88] = four
                    zi = zipfile.ZipInfo(i.filename, date_time=(2026, 9, 30, 12, 0, 0))
                    zi.compress_type = zipfile.ZIP_DEFLATED
                    zi.external_attr = 0o644 << 16
                    out.writestr(zi, bytes(hdr) + body)
                    done += 1
                    print("%s  %dx%d dxgi %d -> %s" % (i.filename, width, height, dxgi, four.decode()))
    print("%s: %d file(s), %.1f MB" % (a.out, done, os.path.getsize(a.out) / 1048576))
    return 0


if __name__ == "__main__":
    sys.exit(main())
