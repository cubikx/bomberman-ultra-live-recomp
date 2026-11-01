// Raw RSX register file plus the streamed-upload stores, shared by the FIFO
// processor (writer) and the renderer (reader). Both run on the RSX thread.
#pragma once

#include "runtime/kernel.h"

namespace rsx {

// One 32-bit value per method offset, exactly as last written by the command stream.
extern u32 g_regs[0x10000 / 4];
inline u32 reg(u32 offset) { return g_regs[offset / 4]; }
inline float regf(u32 offset) { return std::bit_cast<float>(g_regs[offset / 4]); }

// Vertex program microcode (512 instructions x 4 words) and constants (468 vec4),
// filled through TRANSFORM_PROGRAM / TRANSFORM_CONSTANT uploads.
constexpr u32 VP_MAX_INSTRUCTIONS = 512, VP_MAX_CONSTANTS = 468;
extern u32 g_vp_ucode[VP_MAX_INSTRUCTIONS * 4];
extern float g_vp_constants[VP_MAX_CONSTANTS * 4];
extern u64 g_vp_ucode_version;   // bumped on every program upload
extern u64 g_vp_constants_version;   // bumped on every constant upload

// Method offsets the renderer reads (byte offsets of the registers)
namespace m {
constexpr u32 CONTEXT_DMA_COLOR_B = 0x018C, CONTEXT_DMA_COLOR_A = 0x0194, CONTEXT_DMA_ZETA = 0x0198,
              SURFACE_CLIP_HORIZONTAL = 0x0200, SURFACE_CLIP_VERTICAL = 0x0204, SURFACE_FORMAT = 0x0208,
              SURFACE_PITCH_A = 0x020C, SURFACE_COLOR_AOFFSET = 0x0210, SURFACE_ZETA_OFFSET = 0x0214,
              SURFACE_COLOR_BOFFSET = 0x0218, SURFACE_PITCH_B = 0x021C, SURFACE_COLOR_TARGET = 0x0220,
              SURFACE_PITCH_Z = 0x022C, ALPHA_TEST_ENABLE = 0x0304, ALPHA_FUNC = 0x0308, ALPHA_REF = 0x030C,
              BLEND_ENABLE = 0x0310, BLEND_FUNC_SFACTOR = 0x0314, BLEND_FUNC_DFACTOR = 0x0318,
              BLEND_COLOR = 0x031C, BLEND_EQUATION = 0x0320, COLOR_MASK = 0x0324, CLIP_MIN = 0x0394,
              CLIP_MAX = 0x0398, SCISSOR_HORIZONTAL = 0x08C0, SCISSOR_VERTICAL = 0x08C4,
              SHADER_PROGRAM = 0x08E4, VIEWPORT_HORIZONTAL = 0x0A00, VIEWPORT_VERTICAL = 0x0A04,
              VIEWPORT_OFFSET = 0x0A20, VIEWPORT_SCALE = 0x0A30, DEPTH_FUNC = 0x0A6C, DEPTH_MASK = 0x0A70,
              DEPTH_TEST_ENABLE = 0x0A74, TRANSFORM_PROGRAM = 0x0B80, TEXTURE_OFFSET = 0x1A00,
              TEXTURE_FORMAT = 0x1A04, TEXTURE_ADDRESS = 0x1A08, TEXTURE_CONTROL0 = 0x1A0C,
              TEXTURE_CONTROL1 = 0x1A10, TEXTURE_FILTER = 0x1A14, TEXTURE_IMAGE_RECT = 0x1A18,
              TEXTURE_BORDER_COLOR = 0x1A1C, VERTEX_DATA4F = 0x1C00, VERTEX_ARRAY_OFFSET = 0x1680,
              VERTEX_DATA_BASE_OFFSET = 0x1738, VERTEX_DATA_BASE_INDEX = 0x173C, VERTEX_ARRAY_FORMAT = 0x1740,
              BEGIN_END = 0x1808, DRAW_ARRAYS = 0x1814, INDEX_ARRAY_ADDRESS = 0x181C,
              INDEX_ARRAY_DMA = 0x1820, DRAW_INDEX_ARRAY = 0x1824, FRONT_FACE = 0x1834, CULL_FACE = 0x1830,
              CULL_FACE_ENABLE = 0x183C, TEXTURE_CONTROL3 = 0x1840, SHADER_CONTROL = 0x1D60,
              SHADER_WINDOW = 0x1D88, ZSTENCIL_CLEAR_VALUE = 0x1D8C, COLOR_CLEAR_VALUE = 0x1D90,
              CLEAR_SURFACE = 0x1D94, RESTART_INDEX_ENABLE = 0x1DAC, RESTART_INDEX = 0x1DB0,
              TRANSFORM_PROGRAM_LOAD = 0x1E9C, TRANSFORM_PROGRAM_START = 0x1EA0,
              TRANSFORM_CONSTANT_LOAD = 0x1EFC, TRANSFORM_CONSTANT = 0x1F00, VERTEX_ATTRIB_INPUT_MASK = 0x1FF0,
              VERTEX_ATTRIB_OUTPUT_MASK = 0x1FF4;
// texture unit n registers are at TEXTURE_* + n * 0x20; TEXTURE_CONTROL3 + n * 4
}

} // namespace rsx
