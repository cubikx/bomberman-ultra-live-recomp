// Loads the segments of the user's decrypted EBOOT.elf into guest memory.
#include <fstream>
#include <vector>

#include "runtime/kernel.h"
#include "sha1.h"

namespace {

u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u64 be64(const u8* p) { return u64(be32(p)) << 32 | be32(p + 4); }

} // namespace

ImageInfo load_image(const std::filesystem::path& eboot) {
	std::ifstream f(eboot, std::ios::binary);
	if (!f) fatal("cannot open %s", eboot.string().c_str());
	std::vector<u8> d((std::istreambuf_iterator<char>(f)), {});

	std::string digest = sha1_hex(d.data(), d.size());
	if (digest != g_image_sha1)
		fatal("%s does not match the executable this build was recompiled from\n"
		      "  expected sha1 %s\n  got      sha1 %s",
		      eboot.string().c_str(), g_image_sha1, digest.c_str());

	if (d.size() < 64 || std::memcmp(d.data(), "\x7f" "ELF\x02\x02", 6) != 0)
		fatal("%s is not a decrypted PS3 executable", eboot.string().c_str());

	ImageInfo info;
	u64 phoff = be64(&d[32]);
	u16 phentsize = be16(&d[54]), phnum = be16(&d[56]);
	for (u16 i = 0; i < phnum; i++) {
		const u8* ph = &d[phoff + u64(i) * phentsize];
		u32 type = be32(ph), off = u32(be64(ph + 8)), vaddr = u32(be64(ph + 16));
		u32 filesz = u32(be64(ph + 32)), memsz = u32(be64(ph + 40));
		if (type == 1 && memsz) {
			vm::commit(vaddr, memsz);
			vm::reserve(vaddr, memsz);
			std::memcpy(vm_ptr(vaddr), &d[off], filesz);
			std::memset(vm_ptr(vaddr + filesz), 0, memsz - filesz);
			info.data_end = std::max(info.data_end, vaddr + memsz);
			LOG_INFO("loader: segment 0x%08x-0x%08x (file 0x%x)", vaddr, vaddr + memsz, filesz);
		} else if (type == 7) {
			info.tls_addr = vaddr;
			info.tls_filesz = filesz;
			info.tls_memsz = memsz;
		} else if (type == 0x60000001) {
			const u8* p = &d[off];
			// sys_process_param_t: size, magic, version, sdk_version, prio, stacksize, malloc_pagesize, ppc_seg
			info.primary_prio = be32(p + 16);
			info.primary_stacksize = be32(p + 20);
			info.malloc_pagesize = be32(p + 24);
		}
	}
	// The stack size field is either a byte count or one of the SDK's size codes.
	switch (info.primary_stacksize) {
	case 0x10: info.primary_stacksize = 32 * 1024; break;
	case 0x20: info.primary_stacksize = 64 * 1024; break;
	case 0x30: info.primary_stacksize = 96 * 1024; break;
	case 0x40: info.primary_stacksize = 128 * 1024; break;
	case 0x50: info.primary_stacksize = 256 * 1024; break;
	case 0x60: info.primary_stacksize = 512 * 1024; break;
	case 0x70: info.primary_stacksize = 1024 * 1024; break;
	default: info.primary_stacksize = std::clamp<u32>((info.primary_stacksize + 0xFFF) & ~0xFFFu, 0x10000, 0x100000);
	}
	LOG_INFO("loader: TLS 0x%08x (file 0x%x, mem 0x%x), main stack 0x%x, prio %u", info.tls_addr,
	         info.tls_filesz, info.tls_memsz, info.primary_stacksize, info.primary_prio);
	return info;
}
