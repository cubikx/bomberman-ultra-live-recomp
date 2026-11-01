// cellSaveData (auto load / auto save): a save directory per dirName under user/saves/.
//
// The game drives the operation through two callbacks: funcStat sees the directory's
// state and decides whether to go on; funcFile is then called repeatedly, each call
// naming one file operation, until it reports it is done. Structures follow the
// SDK's CellSaveData* layouts (big-endian, 32-bit pointers).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "hle/savedata.h"

namespace fs = std::filesystem;

namespace {

constexpr u32 CELL_SAVEDATA_ERROR_CBRESULT = 0x8002b401, CELL_SAVEDATA_ERROR_ACCESS_ERROR = 0x8002b402,
              CELL_SAVEDATA_ERROR_PARAM = 0x8002b404;
constexpr s32 CBRESULT_OK_LAST_NOCONFIRM = 2, CBRESULT_OK_LAST = 1, CBRESULT_OK_NEXT = 0;
constexpr u32 FILEOP_READ = 0, FILEOP_WRITE = 1, FILEOP_DELETE = 2, FILEOP_WRITE_NOTRUNC = 3;
constexpr u32 FILETYPE_SECUREFILE = 0, FILETYPE_NORMALFILE = 1, FILETYPE_ICON0 = 2, FILETYPE_ICON1 = 3,
              FILETYPE_PIC1 = 4, FILETYPE_SND0 = 5;
constexpr u32 RECREATE_YES = 2, RECREATE_YES_RESET_OWNER = 3, RECREATE_MASK = 0xfffeffff;

// CellSaveDataCBResult
constexpr u32 RESULT_SIZE = 20, RESULT_USERDATA = 16;
// CellSaveDataSystemFileParam: title[128], subTitle[128], detail[1024], attribute, ...
constexpr u32 PARAM_SIZE = 1552, PARAM_TITLE = 0, PARAM_SUBTITLE = 128, PARAM_DETAIL = 256;
// CellSaveDataStatGet
constexpr u32 STATGET_SIZE = 1704, SG_HDDFREE = 0, SG_ISNEW = 4, SG_DIR = 8, SG_PARAM = 64, SG_BIND = 1616,
              SG_SIZEKB = 1620, SG_SYSSIZEKB = 1624, SG_FILENUM = 1628, SG_FILELISTNUM = 1632, SG_FILELIST = 1636;
// CellSaveDataDirStat: atime, mtime, ctime (s64), dirName[32]
constexpr u32 DIRSTAT_NAME = 24;
// CellSaveDataStatSet: setParam*, reCreateMode, indicator*
constexpr u32 STATSET_SIZE = 12;
// CellSaveDataFileStat (one fileList entry): fileType, pad, st_size (u64), times, fileName[13]
constexpr u32 FILESTAT_SIZE = 56, FS_TYPE = 0, FS_SIZE = 8, FS_ATIME = 16, FS_MTIME = 24, FS_CTIME = 32, FS_NAME = 40;
// CellSaveDataFileGet: excSize, reserved[64]
constexpr u32 FILEGET_SIZE = 68;
// CellSaveDataFileSet
constexpr u32 FILESET_SIZE = 48, FSET_OP = 0, FSET_TYPE = 8, FSET_NAME = 28, FSET_OFFSET = 32, FSET_SIZE = 36,
              FSET_BUFSIZE = 40, FSET_BUF = 44;
// CellSaveDataSetBuf: dirListMax, fileListMax, reserved[6], bufSize, buf*
constexpr u32 SETBUF_FILELISTMAX = 4, SETBUF_BUFSIZE = 32, SETBUF_BUF = 36;

// The parts of PARAM.SFO the game reads back, kept as plain text.
constexpr const char* META_FILE = "savedata.txt";

savedata::Hook g_hook = nullptr;
savedata::DoneHook g_done_hook = nullptr;

// Host file name for a file the game names by type (content files have fixed names).
std::string file_name_for(u32 type, u32 name_ea) {
	switch (type) {
	case FILETYPE_ICON0: return "ICON0.PNG";
	case FILETYPE_ICON1: return "ICON1.PAM";
	case FILETYPE_PIC1: return "PIC1.PNG";
	case FILETYPE_SND0: return "SND0.AT3";
	default: return name_ea ? std::string(vm::str(name_ea)) : std::string();
	}
}

bool read_file(const fs::path& p, std::vector<u8>& out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), {});
	return true;
}

bool write_file(const fs::path& p, const std::vector<u8>& data) {
	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
		if (!f) return false;
	}
	std::error_code ec;
	fs::rename(tmp, p, ec);   // replaces atomically, so a crash never leaves half a save
	return !ec;
}

s64 unix_time(fs::file_time_type t) {
	using namespace std::chrono;
	return duration_cast<seconds>(clock_cast<system_clock>(t).time_since_epoch()).count();
}

void put_string(u32 ea, u32 max, const std::string& s) {
	std::memset(vm_ptr(ea), 0, max);
	std::memcpy(vm_ptr(ea), s.data(), std::min<size_t>(s.size(), max - 1));
}

// title / subtitle / detail, one per line (detail may span the rest of the file)
void load_meta(const fs::path& dir, u32 param) {
	std::ifstream f(dir / META_FILE);
	std::string title, subtitle, detail, line;
	std::getline(f, title);
	std::getline(f, subtitle);
	while (std::getline(f, line)) detail += (detail.empty() ? "" : "\n") + line;
	put_string(param + PARAM_TITLE, 128, title);
	put_string(param + PARAM_SUBTITLE, 128, subtitle);
	put_string(param + PARAM_DETAIL, 1024, detail);
}

void save_meta(const fs::path& dir, u32 param) {
	std::ofstream f(dir / META_FILE, std::ios::trunc);
	f << vm::str(param + PARAM_TITLE) << '\n' << vm::str(param + PARAM_SUBTITLE) << '\n' << vm::str(param + PARAM_DETAIL) << '\n';
}

// cellSaveDataUserAutoLoad / cellSaveDataUserAutoSave(version, userId, dirName, errDialog,
// setBuf, funcStat, funcFile, container, userdata)
void auto_op_steps(PPUContext& ctx, bool save) {
	std::string dir_name = vm::str(arg32(ctx, 2));
	u32 set_buf = arg32(ctx, 4), func_stat = arg32(ctx, 5), func_file = arg32(ctx, 6);
	u32 userdata = ld32(u32(ctx.r[1]) + 0x70 + 4);   // 9th argument: parameter save area
	fs::path dir = g_paths.saves / dir_name;
	std::error_code ec;
	bool exists = fs::is_directory(dir, ec);
	LOG_INFO("savedata: auto %s '%s' (%s)", save ? "save" : "load", dir_name.c_str(), exists ? "exists" : "new");

	GuestStackAlloc mem(ctx, RESULT_SIZE + STATGET_SIZE + STATSET_SIZE + FILEGET_SIZE + FILESET_SIZE + 64);
	u32 result = mem.addr, stat_get = result + 32, stat_set = stat_get + STATGET_SIZE + 4,
	    file_get = stat_set + 16, file_set = file_get + FILEGET_SIZE + 12;
	std::memset(vm_ptr(mem.addr), 0, mem.size);

	// --- funcStat --------------------------------------------------------------
	u32 list_max = ld32(set_buf + SETBUF_FILELISTMAX), list_buf = ld32(set_buf + SETBUF_BUF);
	list_max = std::min(list_max, ld32(set_buf + SETBUF_BUFSIZE) / FILESTAT_SIZE);
	st32(stat_get + SG_HDDFREE, 40 * 1024 * 1024);
	st32(stat_get + SG_ISNEW, exists ? 0 : 1);
	put_string(stat_get + SG_DIR + DIRSTAT_NAME, 32, dir_name);
	st32(stat_get + SG_BIND, 0);
	u64 total = 0;
	u32 file_num = 0, list_num = 0;
	if (exists) {
		load_meta(dir, stat_get + SG_PARAM);
		auto t = unix_time(fs::last_write_time(dir, ec));
		st64(stat_get + SG_DIR + 0, u64(t));
		st64(stat_get + SG_DIR + 8, u64(t));
		st64(stat_get + SG_DIR + 16, u64(t));
		for (const auto& e : fs::directory_iterator(dir, ec)) {
			std::string name = e.path().filename().string();
			if (!e.is_regular_file() || name == META_FILE) continue;
			u64 size = e.file_size(ec);
			total += size;
			file_num++;
			if (list_num < list_max && list_buf) {
				u32 fe = list_buf + list_num++ * FILESTAT_SIZE;
				std::memset(vm_ptr(fe), 0, FILESTAT_SIZE);
				u32 type = name == "ICON0.PNG" ? FILETYPE_ICON0 : name == "ICON1.PAM" ? FILETYPE_ICON1
				         : name == "PIC1.PNG"  ? FILETYPE_PIC1  : name == "SND0.AT3"  ? FILETYPE_SND0 : FILETYPE_NORMALFILE;
				st32(fe + FS_TYPE, type);
				st64(fe + FS_SIZE, size);
				auto ft = u64(unix_time(e.last_write_time(ec)));
				st64(fe + FS_ATIME, ft);
				st64(fe + FS_MTIME, ft);
				st64(fe + FS_CTIME, ft);
				put_string(fe + FS_NAME, 13, name);
			}
		}
	}
	st32(stat_get + SG_SYSSIZEKB, 35);
	st32(stat_get + SG_SIZEKB, exists ? u32(total / 1024 + 35) : 0);
	st32(stat_get + SG_FILENUM, file_num);
	st32(stat_get + SG_FILELISTNUM, list_num);
	st32(stat_get + SG_FILELIST, list_buf);

	auto call = [&](u32 func, u32 get, u32 set) {
		std::memset(vm_ptr(result), 0, RESULT_USERDATA);
		st32(result + RESULT_USERDATA, userdata);
		ctx.r[3] = result;
		ctx.r[4] = get;
		ctx.r[5] = set;
		call_guest_opd(ctx, func);
		return s32(ld32(result));
	};

	s32 r = call(func_stat, stat_get, stat_set);
	if (r != CBRESULT_OK_NEXT) {
		LOG_INFO("savedata: funcStat ended the operation (%d)", r);
		return ret(ctx, r == CBRESULT_OK_LAST ? CELL_OK : CELL_SAVEDATA_ERROR_CBRESULT);
	}
	if (save) {
		u32 recreate = ld32(stat_set + 4) & RECREATE_MASK;
		if (exists && (recreate == RECREATE_YES || recreate == RECREATE_YES_RESET_OWNER)) {
			for (const auto& e : fs::directory_iterator(dir, ec))
				if (e.is_regular_file()) fs::remove(e.path(), ec);
		}
		fs::create_directories(dir, ec);
		if (ec) {
			LOG_ERROR("savedata: cannot create %s: %s", dir.string().c_str(), ec.message().c_str());
			return ret(ctx, CELL_SAVEDATA_ERROR_ACCESS_ERROR);
		}
		if (u32 param = ld32(stat_set + 0)) save_meta(dir, param);
	} else if (!exists) {
		return ret(ctx, CELL_SAVEDATA_ERROR_CBRESULT);
	}

	// --- funcFile, until the game says it is done ------------------------------
	for (u32 step = 0; step < 1000; step++) {
		std::memset(vm_ptr(file_set), 0, FILESET_SIZE);
		std::memset(vm_ptr(file_get + 4), 0, FILEGET_SIZE - 4);
		r = call(func_file, file_get, file_set);
		if (r == CBRESULT_OK_LAST || r == CBRESULT_OK_LAST_NOCONFIRM) return ret(ctx, CELL_OK);
		if (r != CBRESULT_OK_NEXT) {
			LOG_WARN("savedata: funcFile failed (%d)", r);
			return ret(ctx, CELL_SAVEDATA_ERROR_CBRESULT);
		}
		u32 op = ld32(file_set + FSET_OP), type = ld32(file_set + FSET_TYPE);
		std::string name = file_name_for(type, ld32(file_set + FSET_NAME));
		if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == META_FILE)
			return ret(ctx, CELL_SAVEDATA_ERROR_PARAM);
		fs::path path = dir / name;
		u32 offset = ld32(file_set + FSET_OFFSET), size = ld32(file_set + FSET_SIZE);
		u32 buf = ld32(file_set + FSET_BUF), buf_size = ld32(file_set + FSET_BUFSIZE);
		std::vector<u8> data;
		switch (op) {
		case FILEOP_READ: {
			if (!read_file(path, data)) {
				LOG_WARN("savedata: %s/%s not found", dir_name.c_str(), name.c_str());
				return ret(ctx, CELL_SAVEDATA_ERROR_ACCESS_ERROR);
			}
			if (g_hook) g_hook(dir_name, name, data, false);
			u32 n = offset < data.size() ? u32(std::min<size_t>({data.size() - offset, size, buf_size})) : 0;
			std::memcpy(vm_ptr(buf), data.data() + offset, n);
			vm::note_written(buf, n);
			st32(file_get + 0, n);
			LOG_INFO("savedata: read %s (%u bytes)", name.c_str(), n);
			break;
		}
		case FILEOP_WRITE:
		case FILEOP_WRITE_NOTRUNC: {
			if (op == FILEOP_WRITE_NOTRUNC) read_file(path, data);
			if (op == FILEOP_WRITE) data.clear();
			if (data.size() < size_t(offset) + size) data.resize(size_t(offset) + size);
			std::memcpy(data.data() + offset, vm_ptr(buf), size);
			if (g_hook) g_hook(dir_name, name, data, true);
			if (!write_file(path, data)) {
				LOG_ERROR("savedata: cannot write %s", path.string().c_str());
				return ret(ctx, CELL_SAVEDATA_ERROR_ACCESS_ERROR);
			}
			st32(file_get + 0, size);
			LOG_INFO("savedata: wrote %s (%u bytes)", name.c_str(), size);
			break;
		}
		case FILEOP_DELETE:
			fs::remove(path, ec);
			st32(file_get + 0, 0);
			break;
		default: return ret(ctx, CELL_SAVEDATA_ERROR_PARAM);
		}
	}
	LOG_ERROR("savedata: funcFile never finished");
	ret(ctx, CELL_SAVEDATA_ERROR_CBRESULT);
}

void auto_op(PPUContext& ctx, bool save) {
	std::string dir_name = vm::str(arg32(ctx, 2));
	auto_op_steps(ctx, save);
	if (g_done_hook) g_done_hook(dir_name, save, u32(ctx.r[3]) == CELL_OK);
}

} // namespace

void savedata::set_hook(Hook hook) { g_hook = hook; }
void savedata::set_done_hook(DoneHook hook) { g_done_hook = hook; }

HLE_FUNC(cellSysutil, cellSaveDataUserAutoLoad) { auto_op(ctx, false); }
HLE_FUNC(cellSysutil, cellSaveDataUserAutoSave) { auto_op(ctx, true); }
