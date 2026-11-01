// RSX shader microcode -> GLSL (Vulkan flavour) translation.
#pragma once

#include <string>
#include <vector>

#include "runtime/kernel.h"

namespace rsx::shader {

// Resource layout shared by every translated shader:
//   set 0 binding 0: vertex constants  (vec4 vc[468])
//   set 0 binding 1: fragment constants (vec4 fc[N], read from the program each draw)
//   set 0 binding 2+n: texture unit n (combined image sampler), n < 16
//   push constants: see PUSH_BLOCK
//   vertex outputs / fragment inputs: location n = RSX vertex output register n (1..15)
constexpr u32 MAX_TEXTURE_UNITS = 16;
constexpr u32 MAX_FRAGMENT_CONSTANTS = 256;

// Must match rsx::gpu::PushConstants.
inline constexpr const char* PUSH_BLOCK =
	"layout(push_constant) uniform Push {\n"
	"\tvec4 vp_scale;    // viewport scale (window = ndc * scale + offset)\n"
	"\tvec4 vp_offset;\n"
	"\tvec4 surface;     // x = width, y = height, z = 1 if the window origin is bottom-left, w = window height\n"
	"\tvec4 alpha_test;  // x = compare func (0..7), y = reference, z = enabled, w = render scale\n"
	"} pc;\n";

struct VertexProgram {
	bool ok = false;
	std::string glsl;
	u32 inputs_used = 0;   // bit n = input register n read
	std::string error;
};
// Translate the vertex program whose first instruction is `start` in `ucode`.
VertexProgram translate_vertex(const u32* ucode, u32 start);

enum class TexDim : u8 { tex2d = 0, cube = 1, tex3d = 2 };

struct FragmentProgram {
	bool ok = false;
	std::string glsl;
	std::vector<u32> constant_offsets;   // byte offset of each inline constant, in fc[] order
	u32 textures_used = 0;
	std::string error;
};
// Translate the fragment program at guest address `ea`. `tex_dims` gives each unit's dimensionality.
FragmentProgram translate_fragment(u32 ea, u32 shader_control, const TexDim* tex_dims);

// Fragment programs embed constants that the game rewrites between draws. The
// cache key covers the instructions only; constants are fed through a buffer.
struct FragmentKey {
	u64 hash = 0;
	u32 size = 0;   // bytes, instructions plus inline constants
	u32 textures_used = 0;
};
FragmentKey fragment_key(u32 ea);

// Fragment program words are stored with their 16-bit halves swapped.
inline u32 fp_word(u32 ea) { return std::rotl(ld32(ea), 16); }

} // namespace rsx::shader
