// The Save Custom Settings screen (CSaveMatchScreen) of the Xbox 360 version, which the
// PS3 port cut: Power-Up Settings -> Save Settings opens it; 3 slots, each "Custom
// Setting N" with its arena's picture or "Open Slot"; A saves into the focused slot
// (asking first when it is used), Y deletes it (asking first), B goes back. The scene
// is runtime/data/SaveMatchScreen.xui (added to media.zip by media_patch.cpp); the
// class is native code registered with the game's XUI next to its own screen classes.
#include <format>
#include <string>

#include "ppu/recomp.h"
#include "profiles/custom_arenas.h"
#include "runtime/kernel.h"

namespace {

using custom_arenas::SLOTS;

// The game's XUI and UI helpers.
constexpr u32 XUI_REGISTER_CLASS = 0xd28a0;   // (const XUIClass*, HXUICLASS* out)
constexpr u32 MALLOC = 0x2ac10;
constexpr u32 SEND_MESSAGE = 0xcedc0;         // (hObj, XUIMessage*)
constexpr u32 GET_CHILD = 0xc8458;            // (hParent, id, HXUIOBJ* out)
constexpr u32 SET_TEXT = 0x12dbb0;            // (hObj, text)
constexpr u32 SET_IMAGE = 0xfa1c0;            // (hImage, path)
constexpr u32 SET_FOCUS = 0xca5c8;            // (hObj, user)
constexpr u32 SHOW = 0x12d5c0;                // (hObj, bool)
constexpr u32 STRING = 0xe0ee0;               // (id) -> localized text
constexpr u32 FORMAT_STRING = 0xe1238;        // (buffer, chars, id, number)
constexpr u32 NAVIGATE_BACK = 0x101208;       // (hScene, hBackScene, 253)
constexpr u32 POPUP = 0xeb670;                // (popups, title, text, type, user)
constexpr u32 POPUPS_PTR = 0x1e0378;
constexpr u32 POPUP_RESULT_BASE = 0xe4608;    // scenes' base handler for a popup's answer
constexpr u32 POPUP_OK = 1;                   // the answer (Ok / Cancel popup, Cancel focused)
constexpr u32 POWERUP_VTABLE = 0x1b16a8;      // copied for the base-class entries
constexpr u32 VTABLE_ENTRIES = 16;

// XUI messages (msg+4 id, +8 handled, +16 data) as the game's scenes see them.
constexpr u32 XM_NOTIFY = 14, XM_INIT = 19, XM_POPUP_RESULT = 2028, XM_GET_BACK_SCENE = 27;
constexpr u32 XN_PRESS = 1, XN_SET_FOCUS = 2;

// The object: the game's scene base (+0 vtable, +4 the scene, +8 popup open, +12, +16
// popup; set up by BASE_INIT and used by POPUP), then the elements this screen uses.
constexpr u32 OBJ_SCENE = 4, OBJ_SLOTS = 24, OBJ_IMAGES = 36, OBJ_LINES = 48, OBJ_FRAMES = 60;
constexpr u32 OBJ_SAVE = 72, OBJ_DELETE = 76, OBJ_BACK = 80, OBJ_TOOLTIP = 84, OBJ_SIZE = 88;
constexpr u32 BASE_INIT = 0xe45c0;            // the scenes' common OnInit (object)

u32 g_vtable = 0;
u32 g_scratch = 0;   // guest strings and message buffers
int g_focus = 0;     // slot with the focus
enum class Asking { None, Overwrite, Delete } g_asking = Asking::None;
int g_asking_slot = 0;

u32 call(PPUContext& ctx, u32 fn, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0, u64 e = 0) {
	ctx.r[3] = a;
	ctx.r[4] = b;
	ctx.r[5] = c;
	ctx.r[6] = d;
	ctx.r[7] = e;
	ppu_icall(ctx, fn);
	return u32(ctx.r[3]);
}

// A UTF-16BE copy of `s` in guest memory, one buffer per call site (`slot` 0-7).
u32 wstr(const std::string& s, int slot) {
	u32 ea = g_scratch + 0x100 + u32(slot) * 0x80;
	for (size_t i = 0; i < s.size() && i < 63; i++) st16(u32(ea + 2 * i), u16(u8(s[i])));
	st16(u32(ea + 2 * std::min<size_t>(s.size(), 63)), 0);
	return ea;
}

u32 text(PPUContext& ctx, const char* id) { return call(ctx, STRING, wstr(id, 7)); }

u32 child(PPUContext& ctx, u32 scene, const std::string& id) {
	st32(g_scratch, 0);
	call(ctx, GET_CHILD, scene, wstr(id, 0), g_scratch);
	return ld32(g_scratch);
}

void refresh(PPUContext& ctx, u32 obj) {
	for (int i = 0; i < SLOTS; i++) {
		int32_t arena = custom_arenas::arena(i);
		u32 label = g_scratch + 0x500;
		if (arena >= 0) call(ctx, FORMAT_STRING, label, 40, wstr("IDS_SAVED_CUSTOM_SETTINGS", 1), u64(i + 1));
		else label = text(ctx, "IDS_OPEN_SLOT");
		call(ctx, SET_TEXT, ld32(obj + OBJ_SLOTS + 4 * i), label);
		u32 thumb = arena >= 0 ? custom_arenas::thumbnail(arena) : 0;
		if (thumb) call(ctx, SET_IMAGE, ld32(obj + OBJ_IMAGES + 4 * i), thumb);
		call(ctx, SHOW, ld32(obj + OBJ_IMAGES + 4 * i), thumb != 0);
		call(ctx, SHOW, ld32(obj + OBJ_FRAMES + 4 * i), thumb != 0);
		call(ctx, SET_IMAGE, ld32(obj + OBJ_LINES + 4 * i),
		     wstr(i == g_focus ? "../fe_images/PUlineHL.png" : "../fe_images/PUlineN.png", 3));
	}
}

void init(PPUContext& ctx, u32 obj) {
	call(ctx, BASE_INIT, obj);
	u32 scene = ld32(obj + OBJ_SCENE);
	for (int i = 0; i < SLOTS; i++) {
		st32(obj + OBJ_SLOTS + 4 * i, child(ctx, scene, std::format("btnSlot{}", i + 1)));
		st32(obj + OBJ_IMAGES + 4 * i, child(ctx, scene, std::format("ArenaImage{}", i + 1)));
		st32(obj + OBJ_LINES + 4 * i, child(ctx, scene, std::format("imgLine{}", i + 1)));
		st32(obj + OBJ_FRAMES + 4 * i, child(ctx, scene, std::format("XuiImage{}", i + 1)));
	}
	st32(obj + OBJ_SAVE, child(ctx, scene, "btnSave"));
	st32(obj + OBJ_DELETE, child(ctx, scene, "btnDelete"));
	st32(obj + OBJ_BACK, child(ctx, scene, "btnBack"));
	st32(obj + OBJ_TOOLTIP, child(ctx, scene, "labToolTip"));
	call(ctx, SET_TEXT, scene, text(ctx, "IDS_TITLE_SAVE_CUSTOM_SETTINGS"));
	call(ctx, SET_TEXT, ld32(obj + OBJ_BACK), text(ctx, "IDS_BACK"));
	call(ctx, SET_TEXT, ld32(obj + OBJ_SAVE), text(ctx, "IDS_SAVE"));
	call(ctx, SET_TEXT, ld32(obj + OBJ_DELETE), text(ctx, "IDS_DELETE"));
	call(ctx, SET_TEXT, ld32(obj + OBJ_TOOLTIP), text(ctx, "IDS_SAVE_CUSTOM_SETTINGS_TOOLTIP"));
	g_focus = 0;
	g_asking = Asking::None;
	refresh(ctx, obj);
}

void ask(PPUContext& ctx, Asking what, int slot, const char* title, const char* desc) {
	g_asking = what;
	g_asking_slot = slot;
	u32 title_text = text(ctx, title);
	st32(g_scratch + 4, title_text);   // STRING may reuse its buffer: keep the first one
	u32 desc_text = call(ctx, STRING, wstr(desc, 6));
	call(ctx, POPUP, ld32(POPUPS_PTR), ld32(g_scratch + 4), desc_text, 1, 255);
}

void go_back(PPUContext& ctx, u32 obj) {
	u32 scene = ld32(obj + OBJ_SCENE);
	u32 msg = g_scratch + 0x40, out = g_scratch + 0x60;
	for (u32 i = 0; i < 20; i += 4) st32(msg + i, 0);
	st32(out, 0);
	st32(msg, 20);
	st32(msg + 4, XM_GET_BACK_SCENE);
	st32(msg + 12, 4);
	st32(msg + 16, out);
	call(ctx, SEND_MESSAGE, scene, msg);
	if (u32 back = ld32(out)) call(ctx, NAVIGATE_BACK, scene, back, 253);
}

void press(PPUContext& ctx, u32 obj, u32 source) {
	for (int i = 0; i < SLOTS; i++)
		if (source == ld32(obj + OBJ_SLOTS + 4 * i)) {
			g_focus = i;
			if (custom_arenas::arena(i) >= 0)
				ask(ctx, Asking::Overwrite, i, "IDS_POPUP_OVERWRITE_SETTING_TITLE",
				    "IDS_POPUP_OVERWRITE_SETTING_CONFIRMATION_DESC");
			else {
				custom_arenas::save(i, custom_arenas::pending());
				refresh(ctx, obj);
			}
			return;
		}
	if (source == ld32(obj + OBJ_DELETE)) {
		if (custom_arenas::arena(g_focus) >= 0)
			ask(ctx, Asking::Delete, g_focus, "IDS_POPUP_DELETE_SETTING_TITLE",
			    "IDS_POPUP_DELETE_SETTING_CONFIRMATION_DESC");
	} else if (source == ld32(obj + OBJ_BACK)) {
		go_back(ctx, obj);
	}
}

void answer(PPUContext& ctx, u32 obj, u32 choice) {
	Asking what = g_asking;
	g_asking = Asking::None;
	if (choice != POPUP_OK) return;
	if (what == Asking::Overwrite) custom_arenas::save(g_asking_slot, custom_arenas::pending());
	else if (what == Asking::Delete) custom_arenas::erase(g_asking_slot);
	refresh(ctx, obj);
}

// vtable[1]: every message the scene gets. r3 = object, r4 = XUIMessage.
void process_message(PPUContext& ctx) {
	u32 obj = u32(ctx.r[3]), msg = u32(ctx.r[4]);
	u32 id = ld32(msg + 4), data = ld32(msg + 16);
	PPUContext saved = ctx;
	bool handled = false;
	if (id == XM_INIT) {
		init(ctx, obj);
	} else if (id == XM_NOTIFY && data && ld32(data) == XN_PRESS) {
		press(ctx, obj, ld32(data + 4));
		handled = true;
	} else if (id == XM_NOTIFY && data && ld32(data) == XN_SET_FOCUS) {
		for (int i = 0; i < SLOTS; i++)
			if (ld32(data + 4) == ld32(obj + OBJ_SLOTS + 4 * i) && g_focus != i) {
				g_focus = i;
				refresh(ctx, obj);
			}
	} else if (id == XM_POPUP_RESULT && data) {
		call(ctx, POPUP_RESULT_BASE, obj, data, msg + 8);
		if (g_asking != Asking::None) answer(ctx, obj, ld32(data));
	}
	if (handled) st32(msg + 8, 1);
	saved.r[3] = 0;
	ctx = saved;
}

// XUIClass create(hObj, void** object)
void create_instance(PPUContext& ctx) {
	u32 scene = u32(ctx.r[3]), out = u32(ctx.r[4]);
	PPUContext saved = ctx;
	u32 obj = call(ctx, MALLOC, OBJ_SIZE);
	for (u32 i = 0; i < OBJ_SIZE; i += 4) st32(obj + i, 0);
	st32(obj, g_vtable);
	st32(obj + OBJ_SCENE, scene);
	st32(out, obj);
	saved.r[3] = 0;
	ctx = saved;
}

} // namespace

// The game registers its XUI classes in sub_000E57D8; this is its call for
// CPowerupSettingsScreen (r3 = the XUIClass {size, name, base, proc, create, destroy}).
// CSaveMatchScreen is registered right after, with the same base, proc and destroy.
void hook_register_classes(PPUContext& ctx, PPUFunc original) {
	u32 cls = u32(ctx.r[3]), out = u32(ctx.r[4]);
	original(ctx);
	u64 r3 = ctx.r[3];
	if (!g_scratch) g_scratch = vm::alloc(vm::MAIN_BASE, 0x1000, 16);
	u32 mine = vm::alloc(vm::MAIN_BASE, 0x100, 16);
	g_vtable = mine + 0x40;
	for (u32 i = 0; i < VTABLE_ENTRIES; i++) st32(g_vtable + 4 * i, ld32(POWERUP_VTABLE + 4 * i));
	st32(g_vtable + 4, hle_trampoline(process_message, "CSaveMatchScreen::ProcessMessage"));
	u32 name = mine + 0x80;
	const char* n = "CSaveMatchScreen";
	for (u32 i = 0; n[i]; i++) st16(name + 2 * i, u16(n[i]));
	for (u32 i = 0; i < 28; i += 4) st32(mine + i, ld32(cls + i));
	st32(mine + 4, name);
	st32(mine + 16, hle_trampoline(create_instance, "CSaveMatchScreen::CreateInstance"));
	call(ctx, XUI_REGISTER_CLASS, mine, out);
	LOG_INFO("save screen: registered CSaveMatchScreen");
	ctx.r[3] = r3;
}
