// RSX texture state and texel decoding.
#pragma once

#include <volk.h>

#include <vector>

#include "runtime/kernel.h"

namespace rsx::tex {

struct Info {
	u32 ea = 0;
	u32 width = 0, height = 0, depth = 1;
	u32 format = 0;        // base format code (0x81..0x9F), LN/UN flags removed
	bool linear = false;   // LN: rows with a pitch, else Morton-swizzled
	bool unnormalized = false;
	bool cube = false;
	u32 dimension = 2;
	u32 mipmaps = 1;
	u32 pitch = 0;
	u32 remap = 0xAAE4;    // identity
	u32 address = 0, filter = 0, control0 = 0;
	bool enabled = false;
};

Info read_state(u32 unit);

struct Decoded {
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkComponentMapping swizzle{};
	std::vector<u8> data;   // level 0, tightly packed rows
	u32 guest_size = 0;     // bytes of level 0 in guest memory
};
// false if the format is not supported yet (logged once per format)
bool decode(const Info& t, Decoded& out);

// bytes of level 0 in guest memory (for cache validation)
u32 guest_size(const Info& t);

} // namespace rsx::tex
