# usage (from the project folder): python -I tools/dbg/sheet.py <dir> <out.png> [cols] [scale]
# Contact sheet of the frame_*.bmp files from --dump-frame/--dump-count/--dump-step,
# in frame order, no image library needed (nearest-neighbour downscale, default 1/4).
import sys, glob, re, struct, zlib
fs = sorted(glob.glob(sys.argv[1] + '/frame_*.bmp'), key=lambda f: int(re.findall(r'frame_(\d+)', f)[0]))
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
k = int(sys.argv[4]) if len(sys.argv) > 4 else 4
tiles = []
for f in fs:
    d = open(f, 'rb').read()
    off, w, h = struct.unpack('<I', d[10:14])[0], *struct.unpack('<ii', d[18:26])
    h = abs(h)
    rows = []
    for y in range(0, h, k):
        row = d[off + y * w * 4: off + (y + 1) * w * 4]
        rows.append(b''.join(bytes((row[x * 4 + 2], row[x * 4 + 1], row[x * 4])) for x in range(0, w, k)))
    tiles.append(rows)
    print(f.split('frame_')[-1], file=sys.stderr)
tw, th = len(tiles[0][0]) // 3, len(tiles[0])
n = len(tiles); R = (n + cols - 1) // cols
raw = bytearray()
for r in range(R):
    for y in range(th):
        raw += b'\0'
        for c in range(cols):
            i = r * cols + c
            raw += tiles[i][y] if i < n else bytes(tw * 3)
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', tw * cols, th * R, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
