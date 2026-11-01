// cellGcmSys: libgcm's system side — memory setup, command buffer control, display buffers, flips.
#include <chrono>
#include <thread>

#include "../rsx/rsx.h"

namespace {

constexpr u32 SEGMENT_SIZE = 32 * 1024;   // the default command buffer is used in 32 KB segments
constexpr u32 CELL_GCM_ERROR_FAILURE = 0x802100FF;

u32 g_cmd_begin, g_cmd_segments;   // default command buffer
u32 g_context_addr;                // CellGcmContextData { begin, end, current, callback }

u32 to_io(u32 ea) {
	u32 off = 0;
	if (!rsx::ea_to_offset(ea, off)) fatal("gcm: 0x%08x is not in GPU-visible memory", ea);
	return off;
}

// Called by the game (through the context's callback pointer) when the current
// command buffer segment is full: hand it to the GPU and continue in the next one.
void gcm_callback(PPUContext& ctx) {
	u32 context = arg32(ctx, 0);
	const auto& cfg = rsx::config();
	u32 current = ld32(context + 8);

	st32(cfg.ctrl_addr, to_io(current));   // put: let the GPU run up to here

	u32 seg = (current - g_cmd_begin) / SEGMENT_SIZE;
	seg = (seg + 1) % g_cmd_segments;
	u32 next_begin = g_cmd_begin + seg * SEGMENT_SIZE;
	u32 next_end = next_begin + SEGMENT_SIZE - 4;
	if (seg == 0) next_begin += 4096;      // first 4 KB stay reserved, as in libgcm

	st32(current, 0x20000000 | to_io(next_begin));   // jump to the next segment
	st32(context + 0, next_begin);
	st32(context + 4, next_end);
	st32(context + 8, next_begin);

	// wait for the GPU to leave the segment we are about to overwrite
	for (;;) {
		u32 get = ld32(cfg.ctrl_addr + 4);
		u32 get_ea = cfg.io_address + get;
		if (get_ea < next_begin || get_ea > next_end) break;
		std::this_thread::yield();
	}
	ret(ctx, CELL_OK);
}

u32 g_callback_opd;

} // namespace

// _cellGcmInitBody(context**, cmdSize, ioSize, ioAddress)
HLE_FUNC(cellGcmSys, _cellGcmInitBody) {
	u32 context_out = arg32(ctx, 0), cmd_size = arg32(ctx, 1), io_size = arg32(ctx, 2), io_addr = arg32(ctx, 3);
	LOG_INFO("_cellGcmInitBody(cmdSize 0x%x, ioSize 0x%x, ioAddress 0x%08x)", cmd_size, io_size, io_addr);
	const auto& cfg = rsx::init(io_addr, io_size);

	g_cmd_begin = io_addr;
	g_cmd_segments = std::max<u32>(1, cmd_size / SEGMENT_SIZE);
	if (!g_callback_opd) g_callback_opd = hle_trampoline(gcm_callback, "cellGcmCallback");

	g_context_addr = vm::alloc(vm::MAIN_BASE, 0x1000, 0x1000);
	st32(g_context_addr + 0, io_addr + 4096);                // begin
	st32(g_context_addr + 4, io_addr + SEGMENT_SIZE - 4);    // end (room for the jump)
	st32(g_context_addr + 8, io_addr + 4096);                // current
	st32(g_context_addr + 12, g_callback_opd);               // callback
	st32(context_out, g_context_addr);

	// the GPU starts at the beginning of the command buffer
	st32(cfg.ctrl_addr + 0, 4096);
	st32(cfg.ctrl_addr + 4, 4096);
	if (g_debug_watch_addr) watch_write(g_debug_watch_addr);   // the IO region exists from here on
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellGcmSys, _cellGcmFunc15) { ret(ctx, CELL_OK); }

HLE_FUNC(cellGcmSys, cellGcmGetConfiguration) {
	const auto& cfg = rsx::config();
	u32 out = arg32(ctx, 0);
	st32(out + 0, rsx::LOCAL_BASE);
	st32(out + 4, cfg.io_address);
	st32(out + 8, rsx::LOCAL_SIZE);
	st32(out + 12, cfg.io_size);
	st32(out + 16, 650000000);   // memory clock
	st32(out + 20, 500000000);   // core clock
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellGcmSys, cellGcmGetControlRegister) { ret(ctx, rsx::config().ctrl_addr); }

HLE_FUNC(cellGcmSys, cellGcmGetLabelAddress) {
	ret(ctx, rsx::config().labels_addr + 16 * (arg32(ctx, 0) & 0xFF));
}

HLE_FUNC(cellGcmSys, cellGcmAddressToOffset) {
	u32 off = 0;
	if (!rsx::ea_to_offset(arg32(ctx, 0), off)) return ret(ctx, CELL_GCM_ERROR_FAILURE);
	st32(arg32(ctx, 1), off);
	ret(ctx, CELL_OK);
}

// cellGcmSetDisplayBuffer(id, offset, pitch, width, height)
HLE_FUNC(cellGcmSys, cellGcmSetDisplayBuffer) {
	u32 id = arg32(ctx, 0);
	if (id > 7) return ret(ctx, CELL_EINVAL);
	rsx::set_display_buffer(id, {arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3), arg32(ctx, 4)});
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellGcmSys, cellGcmSetFlipHandler) {
	rsx::set_flip_handler(arg32(ctx, 0));
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellGcmSys, cellGcmSetFlipMode) {
	rsx::set_flip_mode(arg32(ctx, 0));
	ret(ctx, CELL_OK);
}

// _cellGcmSetFlipCommand(context*, id): append a flip to the command buffer
HLE_FUNC(cellGcmSys, _cellGcmSetFlipCommand) {
	u32 context = arg32(ctx, 0), id = arg32(ctx, 1);
	if (ld32(context + 8) + 8 > ld32(context + 4)) {
		// segment full: go through the context's own callback, like libgcm does
		u64 saved[2] = {ctx.r[3], ctx.r[4]};
		ctx.r[3] = context;
		ctx.r[4] = 2;
		call_guest_opd(ctx, ld32(context + 12));
		ctx.r[3] = saved[0];
		ctx.r[4] = saved[1];
	}
	u32 cur = ld32(context + 8);
	st32(cur, (1u << 18) | 0xFEAC);
	st32(cur + 4, id);
	st32(context + 8, cur + 8);
	ret(ctx, CELL_OK);
}

HLE_FUNC(cellGcmSys, cellGcmGetTiledPitchSize) {
	static const u32 pitches[] = {0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00,
	                              0xE00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000,
	                              0x3400, 0x3800, 0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
	                              0xC000, 0xD000, 0xE000, 0x10000};
	u32 size = arg32(ctx, 0);
	for (u32 p : pitches)
		if (p >= size) return ret(ctx, p);
	ret(ctx, 0);
}

// Tiling and Z-cull only change how the GPU lays out memory internally; the
// emulated GPU works on linear memory, so these are accepted and ignored.
HLE_FUNC(cellGcmSys, cellGcmSetTile) { ret(ctx, CELL_OK); }
HLE_FUNC(cellGcmSys, cellGcmSetZcull) { ret(ctx, CELL_OK); }
HLE_FUNC(cellGcmSys, cellGcmSetInvalidateTile) { ret(ctx, CELL_OK); }
