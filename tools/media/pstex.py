# PS3 .texture.ps3 <-> PNG, for the media.zip textures (stdlib only).
# Layout: "SG28PT00", u32 3, u32 size, u32 size, name\0, format byte (0x16 = A8R8G8B8),
# 2 bytes, flags byte, pad to 4, u16 width, u16 height (BE), 4 bytes, pixels.
import struct, zlib


def parse(d):
	e = d.index(b'\0', 20) + 1
	wo = (e + 4 + 3) & ~3
	w, h = struct.unpack('>HH', d[wo:wo + 4])
	return {'name': d[20:e - 1].decode(), 'format': d[e], 'flags': d[e + 3], 'w': w, 'h': h,
	        'wo': wo, 'header': wo + 8}


def to_rgba(d):
	t = parse(d)
	assert t['format'] == 0x16, f"format {t['format']:#x} not A8R8G8B8"
	px = d[t['header']:t['header'] + t['w'] * t['h'] * 4]
	out = bytearray(len(px))
	out[0::4], out[1::4], out[2::4], out[3::4] = px[1::4], px[2::4], px[3::4], px[0::4]
	return t['w'], t['h'], bytes(out)


def from_rgba(template, w, h, rgba):
	# same header as `template` (an A8R8G8B8 texture), with the new size and pixels
	t = parse(template)
	assert t['format'] == 0x16
	px = bytearray(len(rgba))
	px[0::4], px[1::4], px[2::4], px[3::4] = rgba[3::4], rgba[0::4], rgba[1::4], rgba[2::4]
	head = bytearray(template[:t['header']])
	head[t['wo']:t['wo'] + 4] = struct.pack('>HH', w, h)
	size = len(head) + len(px)
	head[12:20] = struct.pack('>II', size, size)
	return bytes(head) + bytes(px)


def read_png(data):
	assert data[:8] == b'\x89PNG\r\n\x1a\n'
	p = 8; idat = b''; plte = trns = None
	while p < len(data):
		n, = struct.unpack('>I', data[p:p + 4]); typ = data[p + 4:p + 8]; c = data[p + 8:p + 8 + n]; p += 12 + n
		if typ == b'IHDR': w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', c)
		elif typ == b'PLTE': plte = c
		elif typ == b'tRNS': trns = c
		elif typ == b'IDAT': idat += c
	assert depth == 8 and inter == 0, (depth, inter)
	ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
	raw = zlib.decompress(idat); stride = w * ch
	rows = []; prev = bytearray(stride); o = 0
	for y in range(h):
		f = raw[o]; line = bytearray(raw[o + 1:o + 1 + stride]); o += 1 + stride
		for i in range(stride):
			a = line[i - ch] if i >= ch else 0; b = prev[i]; c2 = prev[i - ch] if i >= ch else 0
			if f == 1: line[i] = (line[i] + a) & 255
			elif f == 2: line[i] = (line[i] + b) & 255
			elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
			elif f == 4:
				pa, pb, pc = abs(b - c2), abs(a - c2), abs(a + b - 2 * c2)
				line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c2)) & 255
		rows.append(bytes(line)); prev = line
	out = bytearray()
	for r in rows:
		for x in range(w):
			if ctype == 6: out += r[x * 4:x * 4 + 4]
			elif ctype == 2: out += r[x * 3:x * 3 + 3] + b'\xff'
			elif ctype == 0: out += bytes((r[x],) * 3) + b'\xff'
			elif ctype == 4: out += bytes((r[x * 2],) * 3) + r[x * 2 + 1:x * 2 + 2]
			else:
				i = r[x]; out += plte[i * 3:i * 3 + 3] + bytes((trns[i] if trns and i < len(trns) else 255,))
	return w, h, bytes(out)


def write_png(path, w, h, rgba):
	raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
	def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
	open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
	                       + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def read_xus(d):
	# XUIS string table: "XUIS", u16 version, u16 ?, u32 size, u16 count, then count x (value, key), each u16 length + UTF-16BE
	o = d.find(b'XUIS') + 12
	cnt, = struct.unpack('>H', d[o - 2:o])
	out = []
	for _ in range(cnt):
		pair = []
		for _ in range(2):
			n, = struct.unpack('>H', d[o:o + 2]); pair.append(d[o + 2:o + 2 + 2 * n].decode('utf-16-be')); o += 2 + 2 * n
		out.append((pair[1], pair[0]))
	return out


def write_xus(entries):
	# entries: [(key, value)] in table order
	body = bytearray()
	for k, v in entries:
		for s in (v, k):
			e = s.encode('utf-16-be')
			body += struct.pack('>H', len(e) // 2) + e
	return b'XUIS' + struct.pack('>HIH', 0x0100, 12 + len(body), len(entries)) + bytes(body)


def xuiz_files(d, base=0):
	# {path: bytes} of an XUIZ package (XUIResource2.xzp at 0; the 360 xex embeds one)
	assert d[base:base + 4] == b'XUIZ'
	cnt, = struct.unpack('>H', d[base + 0x14:base + 0x16])
	p = base + 0x16; ents = []
	for _ in range(cnt):
		size, off = struct.unpack('>II', d[p:p + 8]); n = d[p + 8]; p += 9
		ents.append((d[p:p + 2 * n].decode('utf-16-be').replace(chr(92), '/').lstrip('./'), size, off)); p += 2 * n
	return {name: d[p + off:p + off + size] for name, size, off in ents}
