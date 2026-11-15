# Read an Xbox 360 XUI scene in its binary form (XUR version 5, "XUIB") and write it as
# the XML scene text (.xui) the PS3 build reads from media.zip.
#
# XUR 5 layout: header "XUIB", u32 version 5, ..., u16 section count at 0x12, then per
# section a 4-char tag, u32 offset and u32 length. STRN: u16 length + UTF-16BE text per
# string (1-based). VECT: 3 floats per vector (0-based). DATA: the element tree:
#   u16 class (STRN index), u8 flags (1: properties, 2: children)
#   flags & 1: u16 property count, then one block per class of the element's hierarchy,
#              base first, empty ones included. A block is a byte
#              (low 3 bits: mask length, rest: count - 1; 0 for none), the mask
#              (big-endian, bit n = the class's property n) and the set properties' values.
#   flags & 2: u32 child count, then the children.
# Values: string = u16 STRN index, float = f32, vector = u32 VECT index, bool = u8,
# integer = u32; all big-endian.
import struct

S, F, V, B, I = 'str', 'float', 'vect', 'bool', 'int'

# Each class's own properties, in bit order. Only those named here can be read; a scene
# using another one is refused rather than guessed.
PROPS = {
	'XuiElement': {0: ('Id', S), 1: ('Width', F), 2: ('Height', F), 3: ('Position', V), 8: ('Pivot', V)},
	'XuiControl': {0: ('ClassOverride', S), 1: ('Visual', S), 3: ('UnfocusedInput', B),
	               6: ('NavUp', S), 7: ('NavDown', S), 10: ('Text', S)},
	'XuiScene': {0: ('DefaultFocus', S)},
	'XuiImage': {0: ('SizeMode', I), 1: ('ImagePath', S)},
	'XuiButton': {0: ('PressKey', I)},
	'XuiCanvas': {}, 'XuiLabel': {}, 'XuiBackButton': {},
}
BASES = {
	'XuiCanvas': ['XuiElement', 'XuiCanvas'],
	'XuiScene': ['XuiElement', 'XuiControl', 'XuiScene'],
	'XuiImage': ['XuiElement', 'XuiImage'],
	'XuiLabel': ['XuiElement', 'XuiControl', 'XuiLabel'],
	'XuiButton': ['XuiElement', 'XuiControl', 'XuiButton'],
	'XuiBackButton': ['XuiElement', 'XuiControl', 'XuiButton', 'XuiBackButton'],
}


class Element:
	def __init__(self, cls):
		self.cls, self.props, self.children = cls, [], []   # props: [name, type, value]

	def get(self, name):
		return next((p[2] for p in self.props if p[0] == name), None)

	def set(self, name, kind, value):
		for p in self.props:
			if p[0] == name:
				p[2] = value
				return
		self.props.append([name, kind, value])

	def drop(self, name):
		self.props = [p for p in self.props if p[0] != name]

	def walk(self):
		yield self
		for c in self.children:
			yield from c.walk()


def read(d):
	assert d[:4] == b'XUIB' and struct.unpack('>I', d[4:8])[0] == 5, 'not an XUR 5 scene'
	n, = struct.unpack('>H', d[0x12:0x14])
	sec = {}
	for i in range(n):
		tag, off, size = struct.unpack('>4sII', d[0x14 + 12 * i:0x20 + 12 * i])
		sec[tag] = (off, size)
	strings, (o, size) = [None], sec[b'STRN']
	p = o
	while p < o + size:
		k, = struct.unpack('>H', d[p:p + 2])
		strings.append(d[p + 2:p + 2 + 2 * k].decode('utf-16-be'))
		p += 2 + 2 * k
	o, size = sec.get(b'VECT', (0, 0))
	vects = [struct.unpack('>3f', d[o + 12 * i:o + 12 * i + 12]) for i in range(size // 12)]
	o, size = sec[b'DATA']
	data, pos = d[o:o + size], 0

	def take(fmt):
		nonlocal pos
		v = struct.unpack_from('>' + fmt, data, pos)
		pos += struct.calcsize('>' + fmt)
		return v[0]

	def value(kind):
		if kind == S: return strings[take('H')]
		if kind == F: return take('f')
		if kind == V: return vects[take('I')]
		if kind == B: return bool(take('B'))
		return take('I')

	def element():
		nonlocal pos
		cls = strings[take('H')]
		flags = take('B')
		assert not flags & ~3, f'{cls}: timelines are not supported'
		e = Element(cls)
		if flags & 1:
			left = take('H')
			for base in BASES[cls]:
				head = take('B')
				if not head: continue
				mask = int.from_bytes(data[pos:pos + (head & 7)], 'big')
				pos += head & 7
				for bit in range(8 * (head & 7)):
					if not mask >> bit & 1: continue
					assert bit in PROPS[base], f'{cls}: unknown {base} property {bit}'
					name, kind = PROPS[base][bit]
					e.props.append([name, kind, value(kind)])
					left -= 1
			assert not left, f'{cls}: {left} properties not read'
		if flags & 2:
			e.children = [element() for _ in range(take('I'))]
		return e

	root = element()
	assert not any(data[pos:]), f'data after the scene at {pos} of {len(data)}'
	root.version = d[0x0c:0x0e].hex()   # the scene text's version attribute
	return root


def tidy(e):
	# put an element's properties in the order the format gives them (after edits)
	order = [p[0] for base in BASES[e.cls] for _, p in sorted(PROPS[base].items())]
	e.props.sort(key=lambda p: order.index(p[0]))


def fmt(kind, v):
	if kind == F: return f'{v:f}'
	if kind == V: return ','.join(f'{c:f}' for c in v)
	if kind == B: return 'true' if v else 'false'
	return str(v)


def write(root):
	out = []

	def element(e, top=False):
		out.append(f'<{e.cls} version="{root.version}">' if top else f'<{e.cls}>')
		if e.props:
			out.append('<Properties>')
			out.extend(f'<{n}>{fmt(k, v)}</{n}>' for n, k, v in e.props)
			out.append('</Properties>')
		for c in e.children:
			element(c)
		out.append(f'</{e.cls}>')

	element(root, True)
	return ('\r\n'.join(out) + '\r\n').encode('utf-8')
