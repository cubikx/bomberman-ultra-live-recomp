# Debug helper: render a .texture.ps3 (A8R8G8B8, 36-byte header) from media.zip as PNG,
# both as a linear image and as a Morton-swizzled one.
# usage: python -I tools/dbg/texview.py <name in media.zip> <output prefix>
import sys, zipfile, zlib, struct


def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def morton_table(n, bits_self, bits_other, self_first):
    # offset contribution of coordinate value v when x/y bits are interleaved
    t = []
    for v in range(n):
        o = 0; b = 0
        for i in range(max(bits_self, bits_other)):
            for who in ((0, 1) if self_first else (1, 0)):
                if who == 0 and i < bits_self:
                    o |= ((v >> i) & 1) << b
                if (who == 0 and i < bits_self) or (who == 1 and i < bits_other):
                    b += 1
        t.append(o)
    return t


name, out = sys.argv[1], sys.argv[2]
d = zipfile.ZipFile('NPUB30051/USRDIR/media.zip').read(name)
w, h = struct.unpack('>HH', d[28:32])
px = d[36:]
xb, yb = w.bit_length() - 1, h.bit_length() - 1
tx, ty = morton_table(w, xb, yb, True), morton_table(h, yb, xb, False)
for mode in ('linear', 'swizzled'):
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            i = (y * w + x if mode == 'linear' else tx[x] | ty[y]) * 4
            j = (y * w + x) * 3
            rgb[j:j + 3] = px[i + 1:i + 4]
    png(f'{out}_{mode}.png', w, h, bytes(rgb))
print(w, h)
