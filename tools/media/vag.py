# PlayStation 4-bit ADPCM (VAG) encode / decode and a resampler, stdlib only.
# Frame: 16 bytes = (filter << 4 | shift), flags, 14 bytes of 28 signed nibbles (low first).
# Flags: 1 = last frame of the sound, 7 = stop (silent frame after the end).
import math

FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def _clamp(v):
	return -32768 if v < -32768 else 32767 if v > 32767 else v


def _try(block, f, shift, s1, s2):
	f0, f1 = FILTERS[f]
	err = 0
	nibbles = []
	for x in block:
		pred = (s1 * f0 + s2 * f1 + 32) >> 6
		n = ((x - pred) * (1 << shift) + 2048) >> 12 if shift < 12 else 0
		n = -8 if n < -8 else 7 if n > 7 else n
		y = _clamp(((n << 12) >> shift) + pred)
		err += (x - y) * (x - y)
		nibbles.append(n & 15)
		s2, s1 = s1, y
	return err, nibbles, s1, s2


def encode(samples):
	# 16-bit mono samples -> frames (without the leading silent / trailing stop frames)
	samples = list(samples) + [0] * (-len(samples) % 28)
	out = bytearray()
	s1 = s2 = 0
	for i in range(0, len(samples), 28):
		block = samples[i:i + 28]
		best = None
		for f in range(5):
			for shift in range(13):
				r = _try(block, f, shift, s1, s2)
				if best is None or r[0] < best[0][0]:
					best = (r, f, shift)
		(err, nibbles, s1, s2), f, shift = best
		out += bytes((f << 4 | shift, 0)) + bytes(nibbles[k] | nibbles[k + 1] << 4 for k in range(0, 28, 2))
	return out


def decode(frames):
	s1 = s2 = 0
	out = []
	for i in range(0, len(frames), 16):
		f, shift = frames[i] >> 4, frames[i] & 15
		f0, f1 = FILTERS[f] if f < 5 else (0, 0)
		for b in frames[i + 2:i + 16]:
			for n in (b & 15, b >> 4):
				n = n - 16 if n >= 8 else n
				y = _clamp(((n << 12) >> shift) + ((s1 * f0 + s2 * f1 + 32) >> 6))
				out.append(y)
				s2, s1 = s1, y
	return out


def resample(samples, rate_in, rate_out, taps=16):
	# windowed-sinc (Blackman), cutoff a little under the lower Nyquist
	ratio = rate_out / rate_in
	fc = min(1.0, ratio) * 0.95
	n_out = int(len(samples) * ratio)
	out = []
	for n in range(n_out):
		t = n / ratio
		c = int(t)
		acc = 0.0
		for k in range(c - taps + 1, c + taps + 1):
			if 0 <= k < len(samples):
				x = t - k
				w = 0.42 + 0.5 * math.cos(math.pi * x / taps) + 0.08 * math.cos(2 * math.pi * x / taps)
				s = fc if x == 0 else math.sin(math.pi * fc * x) / (math.pi * x)
				acc += samples[k] * s * w
		out.append(acc)
	return out
