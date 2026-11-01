// Minimal SHA-1, used only to check that the EBOOT matches the recompiled build.
#pragma once

#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

inline std::string sha1_hex(const uint8_t* data, size_t len) {
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	uint64_t bitlen = uint64_t(len) * 8;
	size_t total = ((len + 8) / 64 + 1) * 64;
	auto byte_at = [&](size_t i) -> uint8_t {
		if (i < len) return data[i];
		if (i == len) return 0x80;
		if (i >= total - 8) return uint8_t(bitlen >> (8 * (total - 1 - i)));
		return 0;
	};
	for (size_t chunk = 0; chunk < total; chunk += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = uint32_t(byte_at(chunk + 4 * i)) << 24 | uint32_t(byte_at(chunk + 4 * i + 1)) << 16 |
			       uint32_t(byte_at(chunk + 4 * i + 2)) << 8 | byte_at(chunk + 4 * i + 3);
		for (int i = 16; i < 80; i++) w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
			e = d; d = c; c = std::rotl(b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	char out[41];
	for (int i = 0; i < 5; i++) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
	return out;
}
