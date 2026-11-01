// HLE registry: binds the game's imported system functions to host implementations.
#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "runtime/kernel.h"

namespace {

std::map<std::string, HLEFunc>& registry() {
	static std::map<std::string, HLEFunc> r;
	return r;
}

std::unique_ptr<HLEFunc[]> g_bound;
std::unique_ptr<std::atomic<bool>[]> g_warned;

} // namespace

HLERegistrar::HLERegistrar(const char* module, const char* name, HLEFunc fn) {
	registry()[std::string(module) + "::" + name] = fn;
}

void hle_bind_imports() {
	g_bound = std::make_unique<HLEFunc[]>(g_hle_import_count);
	g_warned = std::make_unique<std::atomic<bool>[]>(g_hle_import_count);
	size_t bound = 0;
	for (size_t i = 0; i < g_hle_import_count; i++) {
		auto it = registry().find(std::string(g_hle_imports[i].module) + "::" + g_hle_imports[i].name);
		g_bound[i] = it == registry().end() ? nullptr : it->second;
		bound += g_bound[i] != nullptr;
	}
	LOG_INFO("hle: %zu of %zu imported functions implemented", bound, g_hle_import_count);
}

void ppu_hle_call(PPUContext& ctx, u32 index) {
	if (HLEFunc fn = g_bound[index]) {
		fn(ctx);
		return;
	}
	if (!g_warned[index].exchange(true))
		LOG_WARN("hle: unimplemented %s::%s (returning CELL_OK)", g_hle_imports[index].module,
		         g_hle_imports[index].name);
	ret(ctx, CELL_OK);
}
