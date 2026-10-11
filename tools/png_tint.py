"""Mean R, G, B of a PNG (8-bit RGBA, filter 0, as the port's write_png_rgba writes it) over the centre region, and
B - R. For bisecting a red/blue swap (2026-10-10). Usage: python png_tint.py file.png [...]"""
import sys, struct, zlib
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        if t == b'IHDR':
            w, h = struct.unpack('>II', d[pos + 8:pos + 16])
        elif t == b'IDAT':
            idat += d[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + w * 4
    sr = sg = sb = cnt = 0
    for y in range(h // 4, 3 * h // 4, 4):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(w // 4, 3 * w // 4, 4):
            r, g, b = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            sr += r; sg += g; sb += b; cnt += 1
    print('%s: R %.1f G %.1f B %.1f  B-R %.1f' % (path, sr / cnt, sg / cnt, sb / cnt, (sb - sr) / cnt))
