// The game's media.zip with the runtime's own files added: the files in runtime/data/
// (embedded at build, see CMakeLists.txt), e.g. the Save Custom Settings scene the PS3
// port left out, and with `presentation = live` the files in live/ (Bomberman LIVE
// textures and strings made by tools/media/make_live.py). The game reads its menus,
// textures and strings only from media.zip, so it gets a copy with those files appended
// as stored (uncompressed) entries; a file with the name of an original entry replaces
// it in the central directory. Made once in user/cache/ and remade when the original,
// the setting or the added files change.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.h"
#include "runtime/kernel.h"

struct MediaPatchFile {
	const char* name;
	const unsigned char* data;
	size_t size;
};
extern const MediaPatchFile g_media_patch_files[];
extern const size_t g_media_patch_count;

namespace fs = std::filesystem;

namespace {

struct PatchFile {
	std::string name, data;
};

u32 crc32(const unsigned char* p, size_t n) {
	static u32 table[256];
	if (!table[1])
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	u32 c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

void put16(std::string& s, u32 v) {
	s += char(v & 0xFF);
	s += char((v >> 8) & 0xFF);
}
void put32(std::string& s, u32 v) {
	put16(s, v & 0xFFFF);
	put16(s, v >> 16);
}
u32 get16(const std::string& s, size_t o) { return u8(s[o]) | u32(u8(s[o + 1])) << 8; }
u32 get32(const std::string& s, size_t o) { return get16(s, o) | get16(s, o + 2) << 16; }

// The embedded files, then (presentation = live) every file in live/, replacing an
// embedded one of the same name.
std::vector<PatchFile> patch_files() {
	std::vector<PatchFile> files;
	for (size_t i = 0; i < g_media_patch_count; i++)
		files.push_back({g_media_patch_files[i].name,
		                 std::string(reinterpret_cast<const char*>(g_media_patch_files[i].data), g_media_patch_files[i].size)});
	if (config::get().presentation != config::Presentation::Live) return files;
	std::error_code ec;
	size_t live = 0;
	for (const auto& e : fs::directory_iterator(g_paths.live, ec)) {
		if (!e.is_regular_file() || e.path().extension() == ".patch") continue;   // see usrdir_patched_file
		std::ifstream in(e.path(), std::ios::binary);
		PatchFile f{e.path().filename().string(), std::string(std::istreambuf_iterator<char>(in), {})};
		std::erase_if(files, [&](const PatchFile& o) { return o.name == f.name; });
		files.push_back(std::move(f));
		live++;
	}
	if (!live)
		LOG_WARN("media: presentation = live, but %s has no files (make them with tools/media/make_live.py); "
		         "showing Bomberman ULTRA", g_paths.live.string().c_str());
	else LOG_INFO("media: presentation = live, %zu files from %s", live, g_paths.live.string().c_str());
	return files;
}

// What the cached copy was made from: original size + time, and each added file.
std::string stamp_of(const fs::path& original, const std::vector<PatchFile>& files) {
	std::error_code ec;
	std::string s = std::to_string(fs::file_size(original, ec)) + " " +
	                std::to_string(fs::last_write_time(original, ec).time_since_epoch().count());
	for (const PatchFile& f : files)
		s += " " + f.name + ":" + std::to_string(crc32(reinterpret_cast<const unsigned char*>(f.data.data()), f.data.size()));
	return s;
}

bool build(const fs::path& original, const fs::path& out, const std::vector<PatchFile>& files) {
	std::ifstream in(original, std::ios::binary);
	std::string zip((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	// end of central directory: the last "PK\5\6" (a zip comment may follow it)
	size_t eocd = zip.size() >= 22 ? zip.size() - 22 : std::string::npos;
	while (eocd != std::string::npos && get32(zip, eocd) != 0x06054b50) eocd = eocd ? eocd - 1 : std::string::npos;
	if (eocd == std::string::npos) {
		LOG_WARN("media: %s is not a zip, not patched", original.string().c_str());
		return false;
	}
	u32 entries = get16(zip, eocd + 10), cd_size = get32(zip, eocd + 12), cd_offset = get32(zip, eocd + 16);

	// the original central directory without the entries the added files replace
	// (their data stays in the file, unreferenced)
	std::string kept;
	u32 kept_entries = 0;
	for (u32 i = 0, p = cd_offset; i < entries && p + 46 <= cd_offset + cd_size; i++) {
		u32 name_len = get16(zip, p + 28), len = 46 + name_len + get16(zip, p + 30) + get16(zip, p + 32);
		std::string name = zip.substr(p + 46, name_len);
		if (std::none_of(files.begin(), files.end(), [&](const PatchFile& f) { return f.name == name; })) {
			kept.append(zip, p, len);
			kept_entries++;
		}
		p += len;
	}

	std::string locals, centrals;
	u32 offset = cd_offset;
	for (const PatchFile& f : files) {
		const unsigned char* data = reinterpret_cast<const unsigned char*>(f.data.data());
		u32 crc = crc32(data, f.data.size()), size = u32(f.data.size()), name_len = u32(f.name.size());
		std::string local;
		put32(local, 0x04034b50);
		put16(local, 20);   // version needed
		put16(local, 0);    // flags
		put16(local, 0);    // stored
		put32(local, 0);    // time, date
		put32(local, crc);
		put32(local, size);
		put32(local, size);
		put16(local, name_len);
		put16(local, 0);
		local += f.name;
		local += f.data;

		put32(centrals, 0x02014b50);
		put16(centrals, 20);   // made by
		put16(centrals, 20);   // needed
		put16(centrals, 0);
		put16(centrals, 0);
		put32(centrals, 0);
		put32(centrals, crc);
		put32(centrals, size);
		put32(centrals, size);
		put16(centrals, name_len);
		put16(centrals, 0);   // extra
		put16(centrals, 0);   // comment
		put16(centrals, 0);   // disk
		put16(centrals, 0);   // internal attributes
		put32(centrals, 0);   // external attributes
		put32(centrals, offset);
		centrals += f.name;

		offset += u32(local.size());
		locals += local;
	}
	std::string end;
	put32(end, 0x06054b50);
	put32(end, 0);   // disk numbers
	put16(end, kept_entries + u32(files.size()));
	put16(end, kept_entries + u32(files.size()));
	put32(end, u32(kept.size() + centrals.size()));
	put32(end, offset);
	put16(end, 0);   // comment

	std::error_code ec;
	fs::create_directories(out.parent_path(), ec);
	fs::path tmp = out;
	tmp += ".tmp";
	{
		std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
		o.write(zip.data(), cd_offset);
		o << locals;
		o << kept << centrals << end;
		if (!o) return false;
	}
	fs::rename(tmp, out, ec);
	return !ec;
}

} // namespace

// The media.zip the game should read: `original` with the runtime's files added.
fs::path media_patched_zip(const fs::path& original) {
	static std::mutex lock;
	static fs::path result;
	std::lock_guard g(lock);
	if (!result.empty()) return result;
	result = original;
	std::vector<PatchFile> files = patch_files();
	if (files.empty()) return result;
	fs::path out = g_paths.user / "cache" / "media.zip";
	fs::path stamp_file = out;
	stamp_file += ".stamp";
	std::string stamp = stamp_of(original, files), old;
	std::getline(std::ifstream(stamp_file), old);
	std::error_code ec;
	if (old != stamp || !fs::exists(out, ec)) {
		if (!build(original, out, files)) return result;
		std::ofstream(stamp_file, std::ios::trunc) << stamp;
		LOG_INFO("media: wrote %s (+%zu files)", out.string().c_str(), files.size());
	}
	result = out;
	return result;
}

// Another USRDIR file with presentation = live and a live/<name>.patch (the sound bank
// Bman.bnk: the spoken "Bomberman Live!"): a copy in user/cache with the patch applied,
// remade when the original or the patch changes. Patch records, made by
// tools/media/make_live.py: u32 file offset, u32 length, bytes (big-endian).
fs::path usrdir_patched_file(const fs::path& original) {
	if (config::get().presentation != config::Presentation::Live) return original;
	static std::mutex lock;
	static std::map<std::string, fs::path> done;
	std::lock_guard g(lock);
	std::string name = original.filename().string();
	if (auto it = done.find(name); it != done.end()) return it->second;
	fs::path& result = done[name] = original;
	std::error_code ec;
	fs::path patch_path = g_paths.live / (name + ".patch");
	if (!fs::exists(patch_path, ec)) return result;
	std::ifstream pin(patch_path, std::ios::binary);
	std::string patch((std::istreambuf_iterator<char>(pin)), {});

	fs::path out = g_paths.user / "cache" / name, stamp_file = out;
	stamp_file += ".stamp";
	std::string stamp = std::to_string(fs::file_size(original, ec)) + " " +
	                    std::to_string(fs::last_write_time(original, ec).time_since_epoch().count()) + " " +
	                    std::to_string(crc32(reinterpret_cast<const unsigned char*>(patch.data()), patch.size())),
	            old;
	std::getline(std::ifstream(stamp_file), old);
	if (old != stamp || !fs::exists(out, ec)) {
		u64 size = fs::file_size(original, ec);
		fs::path tmp = out;
		tmp += ".tmp";
		fs::create_directories(out.parent_path(), ec);
		if (!fs::copy_file(original, tmp, fs::copy_options::overwrite_existing, ec)) {
			LOG_WARN("media: cannot copy %s: %s", original.string().c_str(), ec.message().c_str());
			return result;
		}
		{
			std::fstream f(tmp, std::ios::in | std::ios::out | std::ios::binary);
			auto be32 = [&](size_t o) { return u32(u8(patch[o])) << 24 | u32(u8(patch[o + 1])) << 16 | u32(u8(patch[o + 2])) << 8 | u8(patch[o + 3]); };
			for (size_t o = 0; o + 8 <= patch.size();) {
				u32 at = be32(o), len = be32(o + 4);
				if (o + 8 + len > patch.size() || u64(at) + len > size) {
					LOG_WARN("media: %s does not fit %s, not applied", patch_path.string().c_str(), name.c_str());
					f.close();
					fs::remove(tmp, ec);
					return result;
				}
				f.seekp(at);
				f.write(patch.data() + o + 8, len);
				o += 8 + len;
			}
			if (!f) return result;
		}
		fs::rename(tmp, out, ec);
		if (ec) return result;
		std::ofstream(stamp_file, std::ios::trunc) << stamp;
		LOG_INFO("media: wrote %s (%s applied)", out.string().c_str(), patch_path.filename().string().c_str());
	}
	result = out;
	return result;
}
