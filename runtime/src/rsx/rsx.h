// RSX (PS3 GPU) emulation: command FIFO, labels/semaphores, surfaces, flips.
#pragma once

#include "runtime/kernel.h"

namespace rsx {

constexpr u32 LOCAL_BASE = 0xC0000000;  // RSX local memory as seen by the PPU
constexpr u32 LOCAL_SIZE = 0x0F900000;   // what libgcm reports to games

// DMA context handles used by SET_CONTEXT_DMA_* methods
constexpr u32 CONTEXT_DMA_LOCAL = 0xFEED0000, CONTEXT_DMA_MAIN = 0xFEED0001;

struct DisplayBuffer {
	u32 offset = 0, pitch = 0, width = 0, height = 0;
};

struct Config {
	u32 io_address = 0, io_size = 0;
	u32 ctrl_addr = 0;     // CellGcmControl { put, get, ref }
	u32 labels_addr = 0;   // 256 labels, 16 bytes apart
};

// Set up memory and start the FIFO thread. io_address/io_size is the main
// memory the game maps for the GPU (it holds the command buffer).
const Config& init(u32 io_address, u32 io_size);
const Config& config();

// Translate an IO-space offset (what the FIFO and main-memory DMA use) to a guest address.
u32 io_to_ea(u32 offset);
// Guest address -> offset in the matching RSX address space; false if not GPU-visible.
bool ea_to_offset(u32 ea, u32& offset);
// Location 0 = RSX local memory, 1 = main memory (IO space) -> guest address.
u32 location_ea(u32 location, u32 offset);

void set_display_buffer(u32 id, const DisplayBuffer& b);
void set_flip_handler(u32 opd);
void set_flip_mode(u32 mode);
void set_dump_frame(u64 n, u64 count = 1, u64 step = 1);   // debugging: save count frames from n, step apart, as BMP
u64 frames();   // frames shown so far

} // namespace rsx
