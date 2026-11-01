// Entry point: set up paths and guest memory, load the EBOOT, run the game.
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <SDL3/SDL_filesystem.h>

#include "config/config.h"
#include "debug/profiler.h"
#include "input/input.h"
#include "profiles/profiles.h"
#include "trophies/trophies.h"
#include "ui/overlay.h"
#include "runtime/kernel.h"
#include "rsx/renderer.h"
#include "rsx/rsx.h"
#include "video/presenter.h"

void log_set_level(LogLevel lvl);
void sysutil_post_event(u64 status, u64 param);

namespace fs = std::filesystem;

static void usage() {
	std::fprintf(stderr,
	             "usage: bomberman [--game <dir>] [--user <dir>] [--live <dir>] [--trace]\n"
	             "  --game   game folder containing USRDIR/EBOOT.elf (default: game beside the executable)\n"
	             "  --user   folder for settings, saves and trophies (default: user beside the executable)\n"
	             "  --live   Bomberman LIVE files for presentation = live (default: live beside the executable)\n");
}

// Install layout, independent of the working directory:
//   bomberman.exe
//   game/   the PS3 game folder's contents (USRDIR/EBOOT.elf, ...)
//   user/   settings/, saves/, trophies/
//   live/   optional: Bomberman LIVE files made by tools/media/make_live.py
static fs::path exe_dir() {
	const char* base = SDL_GetBasePath();   // owned by SDL, ends with a separator
	return base ? fs::path(reinterpret_cast<const char8_t*>(base)) : fs::current_path();
}

int main(int argc, char** argv) {
	fs::path base = exe_dir();
	fs::path game = base / "game", user = base / "user", live = base / "live";
	u32 watch_addr = 0;
	u64 dump_frame = 0, dump_count = 1, dump_step = 1;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--game" && i + 1 < argc) game = argv[++i];
		else if (a == "--user" && i + 1 < argc) user = argv[++i];
		else if (a == "--live" && i + 1 < argc) live = argv[++i];
		else if (a == "--trace") log_set_level(LogLevel::trace);
		else if (a == "--dump-frame" && i + 1 < argc) dump_frame = std::stoull(argv[++i]);
		else if (a == "--dump-count" && i + 1 < argc) dump_count = std::stoull(argv[++i]);
		else if (a == "--dump-step" && i + 1 < argc) dump_step = std::stoull(argv[++i]);
		else if (a == "--dump-shaders") rsx::gpu::set_dump_shaders(true);
		else if (a == "--watch-write" && i + 1 < argc) watch_addr = u32(std::stoul(argv[++i], nullptr, 16));
		else if (a == "--profile" && i + 1 < argc) {
			// <start seconds>:<duration seconds>
			std::string v = argv[++i];
			size_t colon = v.find(':');
			profiler::start(std::stod(v.substr(0, colon)), colon == std::string::npos ? 10.0 : std::stod(v.substr(colon + 1)));
		} else if (a == "--input-script" && i + 1 < argc) {
			if (!input::set_script(argv[++i])) {
				std::fprintf(stderr, "bad --input-script (expected ms:button,ms:button,...)\n");
				return 2;
			}
		} else {
			usage();
			return 2;
		}
	}

	// user/ holds everything the player creates; each part can be backed up or
	// deleted on its own and is recreated empty on the next start.
	g_paths.game = fs::absolute(game);
	if (std::error_code ec; !fs::exists(g_paths.game / "USRDIR" / "EBOOT.elf", ec))
		fatal("Game files not found.\n\nCopy the contents of the game's NPUB30051 folder (USRDIR, ...) into\n%s\n"
		      "or pass --game <folder>.",
		      g_paths.game.string().c_str());
	g_paths.user = fs::absolute(user);
	g_paths.settings = g_paths.user / "settings";
	g_paths.saves = g_paths.user / "saves";
	g_paths.trophies = g_paths.user / "trophies";
	g_paths.live = fs::absolute(live);
	for (const auto& p : {g_paths.settings, g_paths.saves, g_paths.trophies}) {
		std::error_code ec;
		fs::create_directories(p, ec);
		if (ec) fatal("cannot create %s: %s", p.string().c_str(), ec.message().c_str());
	}
	config::load(g_paths.settings / "settings.ini");
	profiles::init();
	trophies::init();
	input::set_listener({
		overlay::menu_button,
		[](uint32_t port, const std::string& key) {
			profiles::controller_connected(port, key);
			overlay::controller_connected(port);
		},
		profiles::controller_disconnected,
		overlay::exit_combo,
	});

	vm::init();
	ImageInfo info = load_image(g_paths.game / "USRDIR" / "EBOOT.elf");
	hle_bind_imports();

	// The window and its event loop own the host main thread; the game runs beside it.
	video::init("Bomberman ULTRA", 1280, 720);
	g_debug_watch_addr = watch_addr;
	if (dump_frame) rsx::set_dump_frame(dump_frame, dump_count, dump_step);
	std::thread game_thread([info] {
		run_main_thread(info);
		video::quit();
	});
	game_thread.detach();

	profiler::register_current_thread("window");
	video::run([] {
		// Ask the game to exit the way the console's XMB does; force it if it doesn't respond.
		constexpr u64 CELL_SYSUTIL_REQUEST_EXITGAME = 0x0101;
		sysutil_post_event(CELL_SYSUTIL_REQUEST_EXITGAME, 0);
		static bool watchdog_started = false;
		if (!watchdog_started) {
			watchdog_started = true;
			std::thread([] {
				std::this_thread::sleep_for(std::chrono::seconds(5));
				LOG_WARN("game did not exit after the close request; exiting");
				std::fflush(stderr);
				std::_Exit(0);
			}).detach();
		}
	});
	std::fflush(stdout);
	std::_Exit(0);
}
