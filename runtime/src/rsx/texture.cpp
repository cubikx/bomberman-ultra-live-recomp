#include "texture.h"

#include <mutex>
#include <set>

#include "regs.h"
#include "rsx.h"

namespace rsx::tex {
namespace {

enum Format : u32 {
	B8 = 0x81, A1R5G5B5 = 0x82, A4R4G4B4 = 0x83, R5G6B5 = 0x84, A8R8G8B8 = 0x85, DXT1 = 0x86, DXT23 = 0x87,
	DXT45 = 0x88, G8B8 = 0x8B, R6G5B5 = 0x8F, X16 = 0x94, Y16_X16 = 0x95, R5G5B5A1 = 0x97,
	W16Z16Y16X16_FLOAT = 0x9A, W32Z32Y32X32_FLOAT = 0x9B, X32_FLOAT = 0x9C, D1R5G5B5 = 0x9D, D8R8G8B8 = 0x9E,
	Y16_X16_FLOAT = 0x9F,
};

u32 bytes_per_texel(u32 f) {
	switch (f) {
	case B8: return 1;
	case A1R5G5B5: case A4R4G4B4: case R5G6B5: case G8B8: case R6G5B5: case X16: case R5G5B5A1:
	case D1R5G5B5: return 2;
	case A8R8G8B8: case Y16_X16: case X32_FLOAT: case D8R8G8B8: case Y16_X16_FLOAT: return 4;
	case W16Z16Y16X16_FLOAT: return 8;
	case W32Z32Y32X32_FLOAT: return 16;
	default: return 0;
	}
}

bool is_dxt(u32 f) { return f == DXT1 || f == DXT23 || f == DXT45; }

// Morton (Z-order) offset of texel (x, y) in a swizzled power-of-two texture
u32 swizzle_index(u32 x, u32 y, u32 log2w, u32 log2h) {
	u32 out = 0, bit = 0;
	for (u32 i = 0; i < std::max(log2w, log2h); i++) {
		if (i < log2w) out |= ((x >> i) & 1u) << bit++;
		if (i < log2h) out |= ((y >> i) & 1u) << bit++;
	}
	return out;
}

u32 log2u(u32 v) { return v ? 31 - std::countl_zero(v) : 0; }

// texel bytes of (x, y) in guest memory
u32 texel_ea(const Info& t, u32 bpp, u32 x, u32 y) {
	if (t.linear) return t.ea + y * t.pitch + x * bpp;
	return t.ea + swizzle_index(x, y, log2u(t.width), log2u(t.height)) * bpp;
}

u8 expand5(u32 v) { return u8((v << 3) | (v >> 2)); }
u8 expand6(u32 v) { return u8((v << 2) | (v >> 4)); }
u8 expand4(u32 v) { return u8(v * 17); }

VkComponentSwizzle remap_component(u32 remap, u32 slot) {
	// slot: 0=A 1=R 2=G 3=B (the order used by the remap register)
	u32 from = (remap >> (slot * 2)) & 3, op = (remap >> (8 + slot * 2)) & 3;
	if (op == 0) return VK_COMPONENT_SWIZZLE_ZERO;
	if (op == 1) return VK_COMPONENT_SWIZZLE_ONE;
	static const VkComponentSwizzle src[] = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
	                                         VK_COMPONENT_SWIZZLE_B};
	return src[from];
}

std::mutex g_warn_lock;
std::set<u32> g_warned;

} // namespace

Info read_state(u32 unit) {
	using namespace m;
	Info t;
	u32 base = unit * 0x20;
	u32 fmt = reg(TEXTURE_FORMAT + base);
	t.control0 = reg(TEXTURE_CONTROL0 + base);
	t.enabled = (t.control0 >> 31) & 1;
	u32 location = (fmt & 3) - 1;
	t.cube = (fmt >> 2) & 1;
	t.dimension = (fmt >> 4) & 0xF;
	u32 f = (fmt >> 8) & 0xFF;
	t.linear = f & 0x20;
	t.unnormalized = f & 0x40;
	t.format = f & ~0x60u;
	t.mipmaps = std::max(1u, (fmt >> 16) & 0xFFFF);
	u32 rect = reg(TEXTURE_IMAGE_RECT + base);
	t.width = rect >> 16;
	t.height = rect & 0xFFFF;
	u32 c3 = reg(TEXTURE_CONTROL3 + unit * 4);
	t.pitch = c3 & 0xFFFFF;
	t.depth = std::max(1u, c3 >> 20);
	t.remap = reg(TEXTURE_CONTROL1 + base) & 0xFFFF;
	t.address = reg(TEXTURE_ADDRESS + base);
	t.filter = reg(TEXTURE_FILTER + base);
	t.ea = location_ea(location & 1, reg(TEXTURE_OFFSET + base));
	if (!t.pitch) t.pitch = t.width * std::max(1u, bytes_per_texel(t.format));
	return t;
}

u32 guest_size(const Info& t) {
	if (is_dxt(t.format)) {
		u32 block = t.format == DXT1 ? 8 : 16;
		return ((t.width + 3) / 4) * ((t.height + 3) / 4) * block;
	}
	u32 bpp = bytes_per_texel(t.format);
	return t.linear ? t.pitch * t.height : t.width * t.height * bpp;
}

bool decode(const Info& t, Decoded& out) {
	out.guest_size = guest_size(t);
	out.swizzle = {remap_component(t.remap, 1), remap_component(t.remap, 2), remap_component(t.remap, 3),
	               remap_component(t.remap, 0)};
	u32 w = t.width, h = t.height;

	if (is_dxt(t.format)) {
		// block layout matches the PC formats; copied as is
		out.format = t.format == DXT1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
		           : t.format == DXT23 ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
		out.data.assign(vm_ptr(t.ea), vm_ptr(t.ea) + out.guest_size);
		return true;
	}

	u32 bpp = bytes_per_texel(t.format);
	if (bpp == 0 || (!t.linear && (w & (w - 1) || h & (h - 1)))) {
		std::lock_guard lock(g_warn_lock);
		if (g_warned.insert(t.format | (t.linear ? 0x100 : 0)).second)
			LOG_WARN("rsx: texture format 0x%02x (%s, %ux%u) not supported yet", t.format,
			         t.linear ? "linear" : "swizzled", w, h);
		return false;
	}

	if (t.format == W16Z16Y16X16_FLOAT || t.format == W32Z32Y32X32_FLOAT || t.format == X32_FLOAT ||
	    t.format == Y16_X16_FLOAT) {
		// float formats: byte-swap each component, keep the layout
		u32 comp = (t.format == W16Z16Y16X16_FLOAT || t.format == Y16_X16_FLOAT) ? 2 : 4;
		out.format = t.format == W16Z16Y16X16_FLOAT ? VK_FORMAT_R16G16B16A16_SFLOAT
		           : t.format == W32Z32Y32X32_FLOAT ? VK_FORMAT_R32G32B32A32_SFLOAT
		           : t.format == X32_FLOAT ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16G16_SFLOAT;
		out.swizzle = {};   // identity for these
		out.data.resize(size_t(w) * h * bpp);
		u8* dst = out.data.data();
		for (u32 y = 0; y < h; y++)
			for (u32 x = 0; x < w; x++) {
				const u8* src = vm_ptr(texel_ea(t, bpp, x, y));
				for (u32 c = 0; c < bpp; c += comp)
					for (u32 b = 0; b < comp; b++) *dst++ = src[c + comp - 1 - b];
			}
		return true;
	}

	// everything else becomes RGBA8 with channels in their natural places; the
	// remap is applied by the image view's component swizzle
	out.format = VK_FORMAT_R8G8B8A8_UNORM;
	out.data.resize(size_t(w) * h * 4);
	u8* dst = out.data.data();
	for (u32 y = 0; y < h; y++) {
		for (u32 x = 0; x < w; x++, dst += 4) {
			u32 ea = texel_ea(t, bpp, x, y);
			u8 a = 255, r = 0, g = 0, b = 0;
			switch (t.format) {
			case B8: a = r = g = b = ld8(ea); break;
			case A8R8G8B8: a = ld8(ea); r = ld8(ea + 1); g = ld8(ea + 2); b = ld8(ea + 3); break;
			case D8R8G8B8: r = ld8(ea + 1); g = ld8(ea + 2); b = ld8(ea + 3); break;
			case G8B8: g = ld8(ea); b = ld8(ea + 1); r = g; a = b; break;
			case R5G6B5: {
				u16 v = ld16(ea);
				r = expand5(v >> 11); g = expand6((v >> 5) & 63); b = expand5(v & 31);
				break;
			}
			case A1R5G5B5: case D1R5G5B5: {
				u16 v = ld16(ea);
				a = t.format == D1R5G5B5 ? 255 : (v >> 15) ? 255 : 0;
				r = expand5((v >> 10) & 31); g = expand5((v >> 5) & 31); b = expand5(v & 31);
				break;
			}
			case R5G5B5A1: {
				u16 v = ld16(ea);
				r = expand5(v >> 11); g = expand5((v >> 6) & 31); b = expand5((v >> 1) & 31); a = (v & 1) ? 255 : 0;
				break;
			}
			case A4R4G4B4: {
				u16 v = ld16(ea);
				a = expand4(v >> 12); r = expand4((v >> 8) & 15); g = expand4((v >> 4) & 15); b = expand4(v & 15);
				break;
			}
			case R6G5B5: {
				u16 v = ld16(ea);
				r = expand6(v >> 10); g = expand5((v >> 5) & 31); b = expand5(v & 31);
				break;
			}
			case X16: a = r = g = b = u8(ld16(ea) >> 8); break;
			case Y16_X16: { u32 v = ld32(ea); r = u8(v >> 24); g = u8(v >> 8); break; }
			default: break;
			}
			dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
		}
	}
	return true;
}

} // namespace rsx::tex
