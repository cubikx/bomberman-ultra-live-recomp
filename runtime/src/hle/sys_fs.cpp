// sys_fs / cellFs: guest file access mapped onto host files.
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "../objects.h"

#ifndef _WIN32
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

namespace fs = std::filesystem;

Paths g_paths;

fs::path media_patched_zip(const fs::path& original);     // media_patch.cpp
fs::path usrdir_patched_file(const fs::path& original);   // media_patch.cpp

fs::path host_path(const std::string& guest) {
	struct Mount {
		const char* prefix;
		fs::path* root;
		const char* sub;
	};
	static fs::path usrdir, cache, fallback;
	usrdir = g_paths.game / "USRDIR";
	cache = g_paths.user / "cache";
	fallback = g_paths.user / "fs";
	const Mount mounts[] = {
		{"/app_home/", &usrdir, ""},
		{"/host_root/", &usrdir, ""},
		{"/dev_hdd0/game/NPUB30051/", &g_paths.game, ""},
		{"/dev_hdd0/home/00000001/savedata/", &g_paths.saves, ""},
		{"/dev_hdd1/", &cache, ""},
	};
	for (const auto& m : mounts) {
		size_t n = std::strlen(m.prefix);
		if (guest.compare(0, n, m.prefix) == 0) {
			fs::path p = *m.root / fs::path(guest.substr(n));
			if (m.root == &usrdir && guest.substr(n) == "media.zip") return media_patched_zip(p);
			if (m.root == &usrdir && guest.substr(n) == "Bman.bnk") return usrdir_patched_file(p);
			return p;
		}
		if (guest + "/" == m.prefix) return *m.root;
	}
	std::string rel = guest;
	while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
	LOG_WARN("fs: unmapped guest path %s -> user/fs/%s", guest.c_str(), rel.c_str());
	return fallback / rel;
}

namespace {

constexpr u32 FS_O_ACCMODE = 3, FS_O_WRONLY = 1, FS_O_RDWR = 2, FS_O_CREAT = 0x40, FS_O_EXCL = 0x80,
              FS_O_TRUNC = 0x200, FS_O_APPEND = 0x400;

struct File : KernelObject {
	std::FILE* f = nullptr;
	fs::path path;
	std::mutex lock;   // the AIO worker and the game's own threads share the handle
	~File() override {
		if (f) std::fclose(f);
	}
};
struct Dir : KernelObject {
	std::vector<std::pair<std::string, bool>> entries; // name, is_dir
	size_t pos = 0;
};

std::FILE* open_host(const fs::path& p, const char* mode) {
#ifdef _WIN32
	std::wstring wmode(mode, mode + std::strlen(mode));
	return _wfopen(p.c_str(), wmode.c_str());
#else
	return std::fopen(p.c_str(), mode);
#endif
}

void write_stat(u32 out, u32 mode, u64 size) {
	std::memset(vm_ptr(out), 0, 52);
	st32(out, mode);
	st64(out + 36, size);
	st64(out + 44, 4096);
}

constexpr u32 FS_S_IFDIR = 0040000, FS_S_IFREG = 0100000;

} // namespace

// cellFsOpen(path, flags, fd*, arg, size)
void fs_open(PPUContext& ctx) {
	std::string guest = vm::str(arg32(ctx, 0));
	u32 flags = arg32(ctx, 1);
	fs::path p = host_path(guest);
	std::error_code ec;
	bool exists = fs::exists(p, ec);
	if ((flags & FS_O_CREAT) && (flags & FS_O_EXCL) && exists) return ret(ctx, CELL_EEXIST);
	if (!exists && !(flags & FS_O_CREAT)) {
		LOG_INFO("fs: open %s -> not found", guest.c_str());
		return ret(ctx, CELL_ENOENT);
	}
	if (exists && fs::is_directory(p, ec)) return ret(ctx, CELL_EISDIR);
	if (flags & FS_O_CREAT) fs::create_directories(p.parent_path(), ec);

	const char* mode = "rb";
	u32 acc = flags & FS_O_ACCMODE;
	if (acc != 0) {
		if (flags & FS_O_APPEND) mode = acc == FS_O_RDWR ? "a+b" : "ab";
		else if ((flags & FS_O_TRUNC) || !exists) mode = acc == FS_O_RDWR ? "w+b" : "wb";
		else mode = "r+b";
	}
	auto f = std::make_shared<File>();
	f->path = p;
	f->f = open_host(p, mode);
	if (!f->f) return ret(ctx, CELL_EIO);
	st32(arg32(ctx, 2), kobjects().add(f));
	LOG_INFO("fs: open %s (flags 0x%x)", guest.c_str(), flags);
	ret(ctx, CELL_OK);
}

// cellFsRead(fd, buf, nbytes, nread*)
void fs_read(PPUContext& ctx) {
	auto f = kobjects().get<File>(arg32(ctx, 0));
	if (!f) return ret(ctx, CELL_EBADF);
	std::lock_guard lock(f->lock);
	size_t n = std::fread(vm_ptr(arg32(ctx, 1)), 1, arg(ctx, 2), f->f);
	vm::note_written(arg32(ctx, 1), u32(n));   // large reads go straight from the kernel
	if (arg32(ctx, 3)) st64(arg32(ctx, 3), n);
	ret(ctx, CELL_OK);
}

void fs_write(PPUContext& ctx) {
	auto f = kobjects().get<File>(arg32(ctx, 0));
	if (!f) return ret(ctx, CELL_EBADF);
	size_t n = std::fwrite(vm_ptr(arg32(ctx, 1)), 1, arg(ctx, 2), f->f);
	if (arg32(ctx, 3)) st64(arg32(ctx, 3), n);
	ret(ctx, CELL_OK);
}

void fs_close(PPUContext& ctx) {
	ret(ctx, kobjects().remove(arg32(ctx, 0)) ? CELL_OK : CELL_EBADF);
}

// cellFsLseek(fd, offset, whence, pos*)
void fs_lseek(PPUContext& ctx) {
	auto f = kobjects().get<File>(arg32(ctx, 0));
	if (!f) return ret(ctx, CELL_EBADF);
	static const int whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
	u32 w = arg32(ctx, 2);
	if (w > 2) return ret(ctx, CELL_EINVAL);
	std::lock_guard lock(f->lock);
	if (_fseeki64(f->f, s64(arg(ctx, 1)), whence[w])) return ret(ctx, CELL_EIO);
	st64(arg32(ctx, 3), u64(_ftelli64(f->f)));
	ret(ctx, CELL_OK);
}

// --- asynchronous reads ----------------------------------------------------
// CellFsAio { u32 fd; u64 offset; u32 buf; u64 size; u64 user_data } (natural alignment)
// A worker thread with its own guest context performs each read and then calls
// the game's completion callback: func(aio*, error, id, transferred).
namespace {

struct AioRequest {
	u32 aio, id, func;
};
std::mutex g_aio_lock;
std::condition_variable g_aio_cv;
std::deque<AioRequest> g_aio_queue;
std::atomic<u32> g_aio_next_id{1};
std::once_flag g_aio_started;

void aio_worker() {
	PPUContext& ctx = attach_guest_context("fs_aio", 0x10000);
	for (;;) {
		AioRequest rq;
		{
			std::unique_lock lock(g_aio_lock);
			g_aio_cv.wait(lock, [] { return !g_aio_queue.empty(); });
			rq = g_aio_queue.front();
			g_aio_queue.pop_front();
		}
		u32 fd = ld32(rq.aio), buf = ld32(rq.aio + 16);
		u64 offset = ld64(rq.aio + 8), size = ld64(rq.aio + 24);
		u32 error = CELL_OK;
		u64 done = 0;
		if (auto f = kobjects().get<File>(fd)) {
			std::lock_guard lock(f->lock);
			if (_fseeki64(f->f, s64(offset), SEEK_SET) == 0) done = std::fread(vm_ptr(buf), 1, size, f->f);
			else error = CELL_EIO;
			vm::note_written(buf, u32(done));
		} else {
			error = CELL_EBADF;
		}
		static std::atomic<int> logged{0};
		if (logged++ < 8) LOG_INFO("fs: aio %u read %llu of %llu bytes at 0x%llx -> 0x%08x", rq.id, (unsigned long long)done,
		          (unsigned long long)size, (unsigned long long)offset, buf);
		if (rq.func) {
			ctx.r[3] = rq.aio;
			ctx.r[4] = error;
			ctx.r[5] = rq.id;
			ctx.r[6] = done;
			call_guest_opd(ctx, rq.func);
		}
	}
}

} // namespace

HLE_FUNC(sys_fs, cellFsAioInit) {
	std::call_once(g_aio_started, [] { std::thread(aio_worker).detach(); });
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_fs, cellFsAioFinish) { ret(ctx, CELL_OK); }

// cellFsAioRead(aio*, id*, func)
HLE_FUNC(sys_fs, cellFsAioRead) {
	std::call_once(g_aio_started, [] { std::thread(aio_worker).detach(); });
	u32 id = g_aio_next_id++;
	st32(arg32(ctx, 1), id);
	{
		std::lock_guard lock(g_aio_lock);
		g_aio_queue.push_back({arg32(ctx, 0), id, arg32(ctx, 2)});
	}
	g_aio_cv.notify_one();
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_fs, cellFsOpen) { fs_open(ctx); }
HLE_FUNC(sys_fs, cellFsRead) { fs_read(ctx); }
HLE_FUNC(sys_fs, cellFsWrite) { fs_write(ctx); }
HLE_FUNC(sys_fs, cellFsClose) { fs_close(ctx); }
HLE_FUNC(sys_fs, cellFsLseek) { fs_lseek(ctx); }

HLE_FUNC(sys_fs, cellFsFsync) {
	auto f = kobjects().get<File>(arg32(ctx, 0));
	if (!f) return ret(ctx, CELL_EBADF);
	std::fflush(f->f);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_fs, cellFsFstat) {
	auto f = kobjects().get<File>(arg32(ctx, 0));
	if (!f) return ret(ctx, CELL_EBADF);
	std::fflush(f->f);
	std::error_code ec;
	write_stat(arg32(ctx, 1), FS_S_IFREG | 0666, fs::file_size(f->path, ec));
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_fs, cellFsUnlink) {
	std::error_code ec;
	ret(ctx, fs::remove(host_path(vm::str(arg32(ctx, 0))), ec) ? CELL_OK : CELL_ENOENT);
}

HLE_FUNC(sys_fs, cellFsOpendir) {
	std::string guest = vm::str(arg32(ctx, 0));
	fs::path p = host_path(guest);
	std::error_code ec;
	if (!fs::is_directory(p, ec)) return ret(ctx, CELL_ENOENT);
	auto d = std::make_shared<Dir>();
	d->entries = {{".", true}, {"..", true}};
	for (const auto& e : fs::directory_iterator(p, ec))
		d->entries.push_back({e.path().filename().string(), e.is_directory()});
	st32(arg32(ctx, 1), kobjects().add(d));
	ret(ctx, CELL_OK);
}

// cellFsReaddir(fd, CellFsDirent*, nread*)
HLE_FUNC(sys_fs, cellFsReaddir) {
	auto d = kobjects().get<Dir>(arg32(ctx, 0));
	if (!d) return ret(ctx, CELL_EBADF);
	u32 out = arg32(ctx, 1);
	if (d->pos >= d->entries.size()) {
		st64(arg32(ctx, 2), 0);
		return ret(ctx, CELL_OK);
	}
	const auto& [name, is_dir] = d->entries[d->pos++];
	std::memset(vm_ptr(out), 0, 258);
	st8(out, is_dir ? 1 : 2);
	st8(out + 1, u8(std::min<size_t>(name.size(), 255)));
	std::memcpy(vm_ptr(out + 2), name.data(), std::min<size_t>(name.size(), 255));
	st64(arg32(ctx, 2), 258);
	ret(ctx, CELL_OK);
}

HLE_FUNC(sys_fs, cellFsClosedir) {
	ret(ctx, kobjects().remove(arg32(ctx, 0)) ? CELL_OK : CELL_EBADF);
}
