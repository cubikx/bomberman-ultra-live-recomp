# Build the executable's icon (bomberman.ico) from the game's own trophy icons: the
# Bomberman head of TROP009.PNG in TROPDIR/*/TROPHY.TRP. Run by the build (CMakeLists.txt);
# the icon is embedded in bomberman.exe, which Windows shows in Explorer and the taskbar.
# usage: python -I make_icon.py <TROPHY.TRP> <out.ico>
# Writes the 240x240 PNG as it is, plus smaller sizes (area-averaged) as 32-bit bitmaps,
# so Windows never has to shrink the big one itself.
import struct
import sys
import zlib

ICON = "TROP009.PNG"
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128)


def trp_file(trp, name):
	if trp[:4] != b"\xdc\xa2\x4d\x00":
		sys.exit(f"make_icon: not a TROPHY.TRP")
	count, size = struct.unpack(">II", trp[0x10:0x18])
	for i in range(count):
		e = trp[0x40 + i * size:0x40 + (i + 1) * size]
		if e[:32].split(b"\0")[0].decode() == name:
			off, length = struct.unpack(">QQ", e[32:48])
			return trp[off:off + length]
	sys.exit(f"make_icon: {name} not in TROPHY.TRP")


# 8-bit RGBA, non-interlaced PNG -> (width, height, rows of RGBA bytes)
def png_rgba(png):
	w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", png[16:29])
	if depth != 8 or ctype != 6 or interlace:
		sys.exit(f"make_icon: {ICON} is not 8-bit non-interlaced RGBA")
	data, p = b"", 8
	while p < len(png):
		n, kind = struct.unpack(">I4s", png[p:p + 8])
		if kind == b"IDAT":
			data += png[p + 8:p + 8 + n]
		p += 12 + n
	raw, stride, rows, prev = zlib.decompress(data), w * 4, [], bytearray(w * 4)
	for y in range(h):
		f, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
		for x in range(stride):
			a = row[x - 4] if x >= 4 else 0
			b = prev[x]
			c = prev[x - 4] if x >= 4 else 0
			if f == 1: row[x] = (row[x] + a) & 255
			elif f == 2: row[x] = (row[x] + b) & 255
			elif f == 3: row[x] = (row[x] + (a + b) // 2) & 255
			elif f == 4:
				pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
				row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
		rows.append(row)
		prev = row
	return w, h, rows


# area-averaged (alpha-weighted) w x h -> n x n, as a bottom-up BGRA icon bitmap
def icon_bitmap(w, h, rows, n):
	out = bytearray()
	for oy in reversed(range(n)):
		y0, y1 = oy * h / n, (oy + 1) * h / n
		for ox in range(n):
			x0, x1 = ox * w / n, (ox + 1) * w / n
			acc = [0.0] * 4
			for y in range(int(y0), min(h, int(y1 + 0.999))):
				wy = min(y + 1, y1) - max(y, y0)
				for x in range(int(x0), min(w, int(x1 + 0.999))):
					k = wy * (min(x + 1, x1) - max(x, x0))
					r, g, b, a = rows[y][x * 4:x * 4 + 4]
					acc[0] += r * a * k; acc[1] += g * a * k; acc[2] += b * a * k; acc[3] += a * k
			area = (x1 - x0) * (y1 - y0)
			a = acc[3] / area
			r, g, b = (acc[i] / acc[3] if acc[3] else 0 for i in range(3))
			out += bytes((round(b), round(g), round(r), round(a)))
	mask = bytes(((n + 31) // 32 * 4) * n)   # all opaque: the alpha channel decides
	return struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, len(out) + len(mask), 0, 0, 0, 0) + out + mask


def main():
	if len(sys.argv) != 3:
		sys.exit("usage: python -I make_icon.py <TROPHY.TRP> <out.ico>")
	png = trp_file(open(sys.argv[1], "rb").read(), ICON)
	w, h, rows = png_rgba(png)
	images = [(n, icon_bitmap(w, h, rows, n)) for n in SIZES] + [(w, png)]
	ico = struct.pack("<HHH", 0, 1, len(images))
	off = 6 + 16 * len(images)
	for n, data in images:
		ico += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), off)
		off += len(data)
	open(sys.argv[2], "wb").write(ico + b"".join(d for _, d in images))


if __name__ == "__main__":
	main()
