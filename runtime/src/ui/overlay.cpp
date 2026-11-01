// Overlay: profile menu and notices, drawn with Dear ImGui after the game's frame.
//
// The menu belongs to the controller that opened it, which steers it. While the menu
// is open every controller is held from the game (input::set_captured), so nothing
// moves behind it; each gets back to the game once its buttons are let go.
// Mouse and keyboard work too. The Trophies page shows each
// person's trophies, with the icons from TROPHY.TRP packed into one texture.
//
// A controller that joins the game's Player Select gets the menu in join mode: it
// must pick a profile (or Guest), cannot take one that another controller is using,
// and cannot close the menu. Joins queue up, one menu at a time; a controller
// waiting its turn stays held until its menu is done. A controller is asked once per
// connection: after its player has chosen, later joins (the next game) don't ask.
//
// From the PS / Guide menu (not a join menu, not during a match) a player may take a
// profile another controller is using, for pads that got mixed up. That controller
// becomes the guest and gets the menu in the same must-choose mode, headed with who
// took its profile; it cannot take one back from there.
//
// Holding the exit combo (settings.ini) opens "Quit game?" for that controller alone;
// Cancel is the default. Quit closes the game the way closing the window does.
#include "ui/overlay.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <vector>

#include "input/input.h"
#include "profiles/profiles.h"
#include "runtime/kernel.h"
#include "trophies/trophies.h"

extern const unsigned char g_overlay_font[];
extern const unsigned int g_overlay_font_size;

namespace overlay {
namespace {

bool g_ready = false;
SDL_Window* g_window = nullptr;
VkFormat g_format;

struct Notice {
	std::string text;
	Uint64 until;
};
std::mutex g_notice_lock;   // notify() is also called from game threads
std::vector<Notice> g_notices;

enum class Screen { Closed, Pick, Name, Trophies, Quit };
Screen g_screen = Screen::Closed;
uint32_t g_owner = 0;        // controller port that opened the menu
bool g_releasing = false;    // menu closed; the next join menu waits until the owner lets go
uint32_t g_held = 0;         // ports this overlay keeps from the game
uint32_t g_answered = 0;     // ports whose player chose a profile since the controller connected
bool g_join_mode = false;    // the menu was opened because g_owner joined: a choice is required
std::deque<uint32_t> g_waiting;   // joined controllers waiting for their menu (window thread)
// Ports whose profile was taken over, waiting for (or in) their menu: port -> what it lost and to whom
struct Bumped {
	std::string profile;
	uint32_t by;
};
std::map<uint32_t, Bumped> g_bumped;
std::mutex g_join_lock;           // player_joined / player_left come from the game thread
std::vector<std::pair<uint32_t, bool>> g_join_events;   // (port, joined)
int g_sel = 0;               // highlighted row (Pick)
std::vector<std::string> g_items;   // Pick rows: profile names, then guest / new / close
bool g_quit_yes = false;     // Quit: focused choice (false = Cancel)
int g_quit_join = -1;        // Quit opened over this port's join menu: asked again on Cancel
std::string g_name;          // Name: text so far
std::string g_error;
int g_key_row = 0, g_key_col = 0;
bool g_lower = false;
// Trophies page
std::vector<std::string> g_people;
int g_person = 0;
std::map<int32_t, int64_t> g_unlocks;   // of g_people[g_person]
int g_trophy_sel = 0;
bool g_trophy_scroll = false;           // bring the selected row into view

// Trophy icons: one texture, a row of ICON_CELL x ICON_CELL cells
constexpr uint32_t ICON_CELL = 240;
struct IconAtlas {
	VkDevice device = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	uint32_t cells = 0;
	std::map<int32_t, uint32_t> cell_of;   // trophy id -> cell
} g_icons;

// Controller edges with key repeat for the d-pad / stick
uint16_t g_prev1 = 0, g_prev2 = 0;
Uint64 g_repeat_at = 0;

constexpr Uint64 NOTICE_MS = 4000;

// The on-screen keyboard. Special keys are spelled out.
const std::vector<std::vector<std::string>> KEYS = {
	{"A", "B", "C", "D", "E", "F", "G", "H", "I", "J"},
	{"K", "L", "M", "N", "O", "P", "Q", "R", "S", "T"},
	{"U", "V", "W", "X", "Y", "Z", "-", "_", "Space"},
	{"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"},
	{"Shift", "Delete", "Done"},
};

std::string slot_label(uint32_t port) { return "Controller " + std::to_string(port + 1); }

std::string profile_label(const std::string& name) { return name.empty() ? "Guest" : name; }

void close_menu() {
	if (g_screen == Screen::Closed) return;
	if (g_screen == Screen::Name) SDL_StopTextInput(g_window);
	if (g_screen == Screen::Quit && g_quit_join >= 0) {
		g_waiting.push_front(uint32_t(g_quit_join));   // its join menu comes back after the prompt
		g_quit_join = -1;
	}
	if (g_join_mode) g_bumped.erase(g_owner);   // closed without choosing (unplugged / left): keeps the guest
	g_screen = Screen::Closed;
	g_join_mode = false;
	g_releasing = true;   // the press that closed the menu must not reach the game
}

// The connected controller that has this profile, other than the menu's (-1: none).
int profile_used_elsewhere(const std::string& name) { return profiles::holder(name, g_owner); }

void open_pick(uint32_t port) {
	g_releasing = false;
	g_owner = port;
	g_screen = Screen::Pick;
	g_items = profiles::list();
	g_error.clear();
	std::string current = profiles::assigned(port);
	auto it = std::find(g_items.begin(), g_items.end(), current);
	g_sel = it != g_items.end() ? int(it - g_items.begin()) : int(g_items.size());   // guest row
	// the press that opened the menu must not also act in it
	input::PadState s = input::raw_pad(port);
	g_prev1 = s.digital1;
	g_prev2 = s.digital2;
}

void open_name() {
	g_screen = Screen::Name;
	g_name.clear();
	g_error.clear();
	g_key_row = g_key_col = 0;
	g_lower = false;
	SDL_StartTextInput(g_window);
}

void open_quit(uint32_t port) {
	if (g_screen == Screen::Quit) return;
	if (g_screen == Screen::Name) SDL_StopTextInput(g_window);
	g_quit_join = g_screen != Screen::Closed && g_join_mode ? int(g_owner) : -1;
	g_join_mode = false;
	g_releasing = false;
	g_owner = port;
	g_screen = Screen::Quit;
	g_quit_yes = false;
	input::PadState s = input::raw_pad(port);
	g_prev1 = s.digital1;
	g_prev2 = s.digital2;
}

// Same as closing the window: the game is asked to exit, so it can save first.
void confirm_quit() {
	LOG_INFO("overlay: quit confirmed on %s", slot_label(g_owner).c_str());
	g_quit_join = -1;
	close_menu();
	notify("Quitting...");
	SDL_Event e{};
	e.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&e);
}

void choose(const std::string& name) {
	if (profiles::in_match()) {
		if (name != profiles::assigned(g_owner)) {
			g_error = "Profiles can't change during a match";
			return;
		}
		g_answered |= 1u << g_owner;
		close_menu();
		return;
	}
	int other = profile_used_elsewhere(name);
	if (g_join_mode && other >= 0) {
		g_error = name + " is playing on " + slot_label(uint32_t(other));
		return;
	}
	profiles::assign(g_owner, name);   // a profile in use elsewhere moves here; that slot becomes the guest
	g_answered |= 1u << g_owner;
	g_bumped.erase(g_owner);
	notify(slot_label(g_owner) + ": " + profile_label(name));
	if (other >= 0) {
		// taken over: the other controller must choose again, first in line
		uint32_t port = uint32_t(other);
		LOG_INFO("overlay: %s took %s from %s", slot_label(g_owner).c_str(), name.c_str(), slot_label(port).c_str());
		g_bumped[port] = Bumped{name, g_owner};
		g_answered &= ~(1u << port);
		std::erase(g_waiting, port);
		g_waiting.push_front(port);
		notify(slot_label(port) + ": " + name + " moved to " + slot_label(g_owner));
	}
	close_menu();
}

// Pick rows after the profiles
int row_guest() { return int(g_items.size()); }
int row_new() { return int(g_items.size()) + 1; }
int row_trophies() { return int(g_items.size()) + 2; }
int row_close() { return int(g_items.size()) + 3; }

void show_person(int i) {
	int n = int(g_people.size());
	g_person = (i % n + n) % n;
	g_unlocks = trophies::unlocks_of(g_people[g_person]);
	g_trophy_sel = 0;
	g_trophy_scroll = true;
}

// Opens on the person playing on the menu's controller.
void open_trophies() {
	g_screen = Screen::Trophies;
	std::string me = trophies::person_on(g_owner);
	g_people = trophies::people();
	auto it = std::find(g_people.begin(), g_people.end(), me);
	if (it == g_people.end()) it = g_people.insert(g_people.end(), me);
	show_person(int(it - g_people.begin()));
}

int pick_rows() { return g_join_mode ? row_new() + 1 : row_close() + 1; }   // join mode: no Trophies / Close

void activate_pick_row(int row) {
	if (row < row_guest()) choose(g_items[row]);
	else if (row == row_guest()) choose("");
	else if (row == row_new()) {
		if (profiles::in_match()) g_error = "Profiles can't change during a match";
		else open_name();
	} else if (row == row_trophies()) open_trophies();
	else close_menu();
}

void type_char(char c) {
	g_error.clear();
	if (g_name.size() >= profiles::MAX_NAME) return;
	if (c == ' ' && g_name.empty()) return;
	g_name += c;
}

void finish_name() {
	std::string name = g_name;
	while (!name.empty() && name.back() == ' ') name.pop_back();
	std::string error;
	if (!profiles::create(name, &error)) {
		g_error = error;
		return;
	}
	SDL_StopTextInput(g_window);
	choose(name);
}

void press_key(const std::string& k) {
	if (k == "Shift") g_lower = !g_lower;
	else if (k == "Delete") {
		if (!g_name.empty()) g_name.pop_back();
	} else if (k == "Done") finish_name();
	else if (k == "Space") type_char(' ');
	else type_char(g_lower && k[0] >= 'A' && k[0] <= 'Z' ? char(k[0] - 'A' + 'a') : k[0]);
}

void move(int dx, int dy) {
	if (g_screen == Screen::Pick) {
		int rows = pick_rows();
		g_sel = (g_sel + dy + rows) % rows;
		g_error.clear();
	} else if (g_screen == Screen::Name) {
		int rows = int(KEYS.size());
		if (dy) {
			g_key_row = (g_key_row + dy + rows) % rows;
			g_key_col = std::min(g_key_col, int(KEYS[g_key_row].size()) - 1);
		}
		if (dx) {
			int cols = int(KEYS[g_key_row].size());
			g_key_col = (g_key_col + dx + cols) % cols;
		}
	} else if (g_screen == Screen::Quit) {
		if (dx) g_quit_yes = !g_quit_yes;
	} else if (g_screen == Screen::Trophies) {
		if (dx) show_person(g_person + dx);
		int rows = int(trophies::list().size());
		if (dy && rows) {
			g_trophy_sel = std::clamp(g_trophy_sel + dy, 0, rows - 1);
			g_trophy_scroll = true;
		}
	}
}

void controller_input() {
	input::PadState s = input::raw_pad(g_owner);
	if (!s.connected) {
		close_menu();
		return;
	}
	// typing on a keyboard arrives as text; its keys also map to pad buttons, so
	// don't let them press on-screen keys at the same time
	if (g_screen == Screen::Name && input::controller_key(g_owner) == "keyboard") {
		int n = 0;
		const bool* keys = SDL_GetKeyboardState(&n);
		if (std::any_of(keys, keys + n, [](bool k) { return k; })) {
			input::PadState idle = input::raw_pad(g_owner);
			g_prev1 = idle.digital1;
			g_prev2 = idle.digital2;
			return;
		}
	}
	uint16_t d1 = s.digital1, d2 = s.digital2;
	// the left stick steers like the d-pad
	if (s.ly < 0x40) d1 |= input::d1::UP;
	if (s.ly > 0xC0) d1 |= input::d1::DOWN;
	if (s.lx < 0x40) d1 |= input::d1::LEFT;
	if (s.lx > 0xC0) d1 |= input::d1::RIGHT;
	uint16_t new1 = d1 & ~g_prev1, new2 = d2 & ~g_prev2;
	Uint64 now = SDL_GetTicks();
	constexpr uint16_t DIRS = input::d1::UP | input::d1::DOWN | input::d1::LEFT | input::d1::RIGHT;
	if (new1 & DIRS) g_repeat_at = now + 350;
	else if ((d1 & DIRS) && now >= g_repeat_at) {
		new1 |= d1 & DIRS;
		g_repeat_at = now + 90;
	}
	g_prev1 = d1;
	g_prev2 = d2;

	int dx = (new1 & input::d1::RIGHT ? 1 : 0) - (new1 & input::d1::LEFT ? 1 : 0);
	int dy = (new1 & input::d1::DOWN ? 1 : 0) - (new1 & input::d1::UP ? 1 : 0);
	if (dx || dy) move(dx, dy);

	if (g_screen == Screen::Pick) {
		if (new2 & input::d2::CROSS) activate_pick_row(g_sel);
		else if ((new2 & input::d2::CIRCLE) && !g_join_mode) close_menu();
	} else if (g_screen == Screen::Name) {
		if (new2 & input::d2::CROSS) press_key(KEYS[g_key_row][g_key_col]);
		else if (new2 & input::d2::SQUARE) press_key("Delete");
		else if (new2 & input::d2::TRIANGLE) press_key("Shift");
		else if (new1 & input::d1::START) finish_name();
		else if (new2 & input::d2::CIRCLE) {
			SDL_StopTextInput(g_window);
			open_pick(g_owner);
		}
	} else if (g_screen == Screen::Quit) {
		if (new2 & input::d2::CROSS) {
			if (g_quit_yes) confirm_quit();
			else close_menu();
		} else if (new2 & input::d2::CIRCLE) close_menu();
	} else if (g_screen == Screen::Trophies) {
		if (new2 & (input::d2::CIRCLE | input::d2::CROSS)) open_pick(g_owner);
		else if (new2 & input::d2::L1) show_person(g_person - 1);
		else if (new2 & input::d2::R1) show_person(g_person + 1);
	}
}

// --- drawing ---------------------------------------------------------------

void draw_notices(float scale) {
	Uint64 now = SDL_GetTicks();
	std::lock_guard lock(g_notice_lock);
	std::erase_if(g_notices, [&](const Notice& n) { return now >= n.until; });
	float y = 24.0f * scale;
	for (size_t i = 0; i < g_notices.size(); i++) {
		const Notice& n = g_notices[i];
		float left = float(n.until - now);
		float alpha = std::clamp(left / 400.0f, 0.0f, 1.0f);
		ImGui::SetNextWindowPos(ImVec2(24.0f * scale, y));
		ImGui::SetNextWindowBgAlpha(0.75f * alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
		std::string id = "##notice" + std::to_string(i);
		ImGui::Begin(id.c_str(), nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
		                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextUnformatted(n.text.c_str());
		y += ImGui::GetWindowHeight() + 8.0f * scale;
		ImGui::End();
		ImGui::PopStyleVar();
	}
}

void hint(const char* text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.75f, 1.0f));
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
}

void draw_pick() {
	auto bumped = g_bumped.find(g_owner);
	if (g_join_mode && bumped != g_bumped.end())
		ImGui::Text("%s took %s from %s. Who's playing?", slot_label(bumped->second.by).c_str(),
		            bumped->second.profile.c_str(), slot_label(g_owner).c_str());
	else if (g_join_mode) ImGui::Text("%s joined. Who's playing?", slot_label(g_owner).c_str());
	else ImGui::Text("Who's playing on %s?", slot_label(g_owner).c_str());
	hint(input::controller_name(g_owner).c_str());
	bool locked = profiles::in_match();
	if (locked) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
		ImGui::TextUnformatted("Match in progress: profiles can change after it.");
		ImGui::PopStyleColor();
	}
	ImGui::Separator();
	std::string current = profiles::assigned(g_owner);
	auto row = [&](int i, const std::string& label, const std::string& note) {
		bool sel = g_sel == i;
		std::string text = label + (note.empty() ? "" : "   " + note) + "##row" + std::to_string(i);
		if (ImGui::Selectable(text.c_str(), sel)) activate_pick_row(i);
		if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0) g_sel = i;
		if (sel) ImGui::SetItemDefaultFocus();
	};
	for (int i = 0; i < int(g_items.size()); i++) {
		std::string note;
		int other = profile_used_elsewhere(g_items[i]);
		if (g_items[i] == current) note = "(current)";
		else if (other >= 0) note = "(" + slot_label(uint32_t(other)) + (g_join_mode || locked ? ")" : ": take over)");
		bool grey = (g_join_mode && other >= 0) || (locked && g_items[i] != current);
		if (grey) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
		row(i, g_items[i], note);
		if (grey) ImGui::PopStyleColor();
	}
	if (locked && !current.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
	row(row_guest(), "Guest", current.empty() ? "(current)" : "");
	if (locked && !current.empty()) ImGui::PopStyleColor();
	if (locked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
	row(row_new(), "+ New profile", "");
	if (locked) ImGui::PopStyleColor();
	if (!g_join_mode) {
		row(row_trophies(), "Trophies", "");
		row(row_close(), "Close", "");
	}
	if (!g_error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
		ImGui::TextUnformatted(g_error.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Separator();
	hint(g_join_mode ? "Cross / A: choose" : "Cross / A: choose     Circle / B: close");
	if (!g_waiting.empty()) {
		std::string next = "Next: ";
		for (size_t i = 0; i < g_waiting.size(); i++) next += (i ? ", " : "") + slot_label(g_waiting[i]);
		hint(next.c_str());
	}
}

void draw_quit(float scale) {
	ImGui::TextUnformatted("Quit game?");
	hint(("Asked by " + slot_label(g_owner)).c_str());
	ImGui::Separator();
	ImGui::Spacing();
	ImVec2 size(220.0f * scale, 64.0f * scale);
	auto choice = [&](const char* label, bool yes) {
		bool sel = g_quit_yes == yes;
		if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.55f, 0.1f, 1.0f));
		if (ImGui::Button(label, size)) {
			if (yes) confirm_quit();
			else close_menu();
		}
		if (sel) ImGui::PopStyleColor();
	};
	choice("Cancel", false);
	ImGui::SameLine();
	choice("Quit", true);
	ImGui::Spacing();
	ImGui::Separator();
	hint("Left / Right: choose   Cross / A: confirm   Circle / B: cancel");
}

void draw_name(float scale) {
	ImGui::TextUnformatted("New profile");
	ImGui::Separator();
	std::string shown = g_name + ((SDL_GetTicks() / 500) % 2 ? "_" : " ");
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
	ImGui::TextUnformatted(shown.c_str());
	ImGui::PopStyleColor();
	if (!g_error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::TextUnformatted(g_error.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Spacing();
	float cell = 46.0f * scale;
	for (int r = 0; r < int(KEYS.size()); r++) {
		for (int c = 0; c < int(KEYS[r].size()); c++) {
			std::string k = KEYS[r][c];
			std::string label = k.size() == 1 && g_lower && k[0] >= 'A' && k[0] <= 'Z' ? std::string(1, char(k[0] - 'A' + 'a')) : k;
			if (k == "Shift" && g_lower) label = "SHIFT";
			bool sel = r == g_key_row && c == g_key_col;
			float w = k.size() > 1 ? cell * (r == 4 ? 3.2f : 2.1f) : cell;
			if (c) ImGui::SameLine();
			if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.55f, 0.1f, 1.0f));
			if (ImGui::Button((label + "##k" + std::to_string(r) + "_" + std::to_string(c)).c_str(), ImVec2(w, cell))) {
				g_key_row = r;
				g_key_col = c;
				press_key(k);
			}
			if (sel) ImGui::PopStyleColor();
		}
	}
	ImGui::Spacing();
	hint("Cross / A: type   Square / X: delete   Triangle / Y: shift   Start: done   Circle / B: back");
	hint("A keyboard works too (Enter: done, Esc: back).");
}

ImVec4 grade_color(trophies::Grade g) {
	switch (g) {
	case trophies::Grade::platinum: return ImVec4(0.7f, 0.85f, 1.0f, 1.0f);
	case trophies::Grade::gold: return ImVec4(1.0f, 0.82f, 0.25f, 1.0f);
	case trophies::Grade::silver: return ImVec4(0.78f, 0.8f, 0.85f, 1.0f);
	default: return ImVec4(0.85f, 0.55f, 0.3f, 1.0f);
	}
}

const char* grade_label(trophies::Grade g) {
	switch (g) {
	case trophies::Grade::platinum: return "Platinum";
	case trophies::Grade::gold: return "Gold";
	case trophies::Grade::silver: return "Silver";
	default: return "Bronze";
	}
}

std::string local_time(int64_t t) {
	std::time_t tt = std::time_t(t);
	char buf[32] = "";
	if (const std::tm* tm = std::localtime(&tt)) std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", tm);
	return buf;
}

void draw_trophies(VkExtent2D extent, float scale) {
	const auto& list = trophies::list();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) show_person(g_person - 1);
	ImGui::SameLine();
	ImGui::TextUnformatted(g_people[g_person].c_str());
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right)) show_person(g_person + 1);
	ImGui::SameLine();
	int got = 0;
	for (const auto& t : list) got += g_unlocks.contains(t.id);
	hint(std::format("   {} of {} trophies", got, list.size()).c_str());
	ImGui::Separator();

	if (list.empty()) {
		hint("No trophy list in the game files (TROPDIR/*/TROPHY.TRP).");
	} else {
		float icon = 84.0f * scale;
		ImGui::BeginChild("##list", ImVec2(extent.width * 0.5f, extent.height * 0.6f));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		for (int i = 0; i < int(list.size()); i++) {
			const trophies::Trophy& t = list[i];
			auto u = g_unlocks.find(t.id);
			bool have = u != g_unlocks.end();
			bool secret = t.hidden && !have;
			// the highlight goes behind the row, so draw the row first on a second channel
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			ImGui::BeginGroup();
			auto cell = g_icons.cell_of.find(t.id);
			if (g_icons.set && cell != g_icons.cell_of.end()) {
				float u0 = float(cell->second) / float(g_icons.cells), u1 = float(cell->second + 1) / float(g_icons.cells);
				ImVec4 tint = have ? ImVec4(1, 1, 1, 1) : ImVec4(0.3f, 0.3f, 0.35f, 0.8f);
				ImGui::Image(ImTextureID(g_icons.set), ImVec2(icon, icon), ImVec2(u0, 0), ImVec2(u1, 1), tint);
			} else {
				ImGui::Dummy(ImVec2(icon, icon));
			}
			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::PushStyleColor(ImGuiCol_Text, have ? grade_color(t.grade) : ImVec4(0.6f, 0.6f, 0.65f, 1.0f));
			ImGui::TextUnformatted(secret ? "Hidden trophy" : t.name.c_str());
			ImGui::PopStyleColor();
			ImGui::PushTextWrapPos(0.0f);   // wrap at the list's edge
			hint(secret ? "Keep playing to find out." : t.detail.c_str());
			ImGui::PopTextWrapPos();
			std::string status = std::string(grade_label(t.grade)) + "   " +
			                     (have ? "Unlocked " + local_time(u->second) : std::string("Locked"));
			ImGui::PushStyleColor(ImGuiCol_Text, have ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f) : ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
			ImGui::TextUnformatted(status.c_str());
			ImGui::PopStyleColor();
			ImGui::EndGroup();
			ImGui::EndGroup();
			ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
			b.x = right;
			dl->ChannelsSetCurrent(0);
			if (i == g_trophy_sel) {
				dl->AddRectFilled(ImVec2(a.x - 6 * scale, a.y - 6 * scale), ImVec2(b.x, b.y + 6 * scale),
				                  IM_COL32(242, 140, 25, 70), 6.0f * scale);
				if (g_trophy_scroll) {
					ImGui::SetScrollHereY(0.5f);
					g_trophy_scroll = false;
				}
			}
			dl->ChannelsMerge();
			if (ImGui::IsMouseHoveringRect(a, b) && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0)
				g_trophy_sel = i;
			ImGui::Dummy(ImVec2(0, 6 * scale));
		}
		ImGui::EndChild();
	}
	ImGui::Separator();
	hint("Left / Right or L1 / R1: other people     Up / Down: scroll     Circle / B: back");
}

// --- trophy icons ------------------------------------------------------------------

uint32_t find_memory(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(gpu, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
	return UINT32_MAX;
}

// Decode the icons side by side into one RGBA strip and upload it. Init only: the
// renderer does not share the queue yet.
void load_icons(const VulkanInfo& vk) {
	std::vector<std::pair<int32_t, stbi_uc*>> decoded;
	std::vector<std::pair<int, int>> sizes;
	for (const auto& t : trophies::list()) {
		const std::string& png = trophies::icon_png(t.id);
		int w = 0, h = 0, comp = 0;
		if (png.empty()) continue;
		stbi_uc* img = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), int(png.size()), &w, &h, &comp, 4);
		if (!img) continue;
		decoded.push_back({t.id, img});
		sizes.push_back({w, h});
	}
	if (decoded.empty()) return;
	uint32_t n = uint32_t(decoded.size()), width = n * ICON_CELL;
	std::vector<uint8_t> strip(size_t(width) * ICON_CELL * 4, 0);
	for (uint32_t c = 0; c < n; c++) {
		auto [w, h] = sizes[c];   // all 240x240 in this game; anything else is cropped
		for (uint32_t y = 0; y < std::min<uint32_t>(ICON_CELL, h); y++)
			std::memcpy(&strip[(size_t(y) * width + c * ICON_CELL) * 4], decoded[c].second + size_t(y) * w * 4,
			            std::min<uint32_t>(ICON_CELL, w) * 4);
		g_icons.cell_of[decoded[c].first] = c;
		stbi_image_free(decoded[c].second);
	}
	g_icons.cells = n;

	VkDevice dev = vk.device;
	g_icons.device = dev;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {width, ICON_CELL, 1};
	ici.mipLevels = ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	vkCreateImage(dev, &ici, nullptr, &g_icons.image);
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, g_icons.image, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory(vk.gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vkAllocateMemory(dev, &mai, nullptr, &g_icons.memory);
	vkBindImageMemory(dev, g_icons.image, g_icons.memory, 0);

	VkBuffer staging;
	VkDeviceMemory staging_mem;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = strip.size();
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	vkCreateBuffer(dev, &bci, nullptr, &staging);
	vkGetBufferMemoryRequirements(dev, staging, &req);
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory(vk.gpu, req.memoryTypeBits,
	                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vkAllocateMemory(dev, &mai, nullptr, &staging_mem);
	vkBindBufferMemory(dev, staging, staging_mem, 0);
	void* map = nullptr;
	vkMapMemory(dev, staging_mem, 0, strip.size(), 0, &map);
	std::memcpy(map, strip.data(), strip.size());
	vkUnmapMemory(dev, staging_mem);

	VkCommandPool pool;
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pci.queueFamilyIndex = vk.queue_family;
	vkCreateCommandPool(dev, &pci, nullptr, &pool);
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	vkAllocateCommandBuffers(dev, &cai, &cmd);
	VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &cbi);
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = g_icons.image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
	                     1, &b);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = ici.extent;
	vkCmdCopyBufferToImage(cmd, staging, g_icons.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
	                     nullptr, 1, &b);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(vk.queue);
	vkDestroyCommandPool(dev, pool, nullptr);
	vkDestroyBuffer(dev, staging, nullptr);
	vkFreeMemory(dev, staging_mem, nullptr);

	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = g_icons.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ici.format;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCreateImageView(dev, &vci, nullptr, &g_icons.view);
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = 1.0f;
	vkCreateSampler(dev, &sci, nullptr, &g_icons.sampler);
	g_icons.set = ImGui_ImplVulkan_AddTexture(g_icons.sampler, g_icons.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	LOG_INFO("overlay: %u trophy icons loaded", n);
}

void free_icons() {
	if (!g_icons.device) return;
	if (g_icons.set) ImGui_ImplVulkan_RemoveTexture(g_icons.set);
	vkDestroySampler(g_icons.device, g_icons.sampler, nullptr);
	vkDestroyImageView(g_icons.device, g_icons.view, nullptr);
	vkDestroyImage(g_icons.device, g_icons.image, nullptr);
	vkFreeMemory(g_icons.device, g_icons.memory, nullptr);
	g_icons = {};
}

} // namespace

void init(SDL_Window* window, const VulkanInfo& vk) {
	g_window = window;
	g_format = vk.format;
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;   // nothing to remember between runs
	io.LogFilename = nullptr;
	ImFontConfig fc;
	fc.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(g_overlay_font), int(g_overlay_font_size), 40.0f, &fc);

	ImGuiStyle& st = ImGui::GetStyle();
	ImGui::StyleColorsDark();
	st.WindowRounding = 10.0f;
	st.FrameRounding = 6.0f;
	st.WindowPadding = ImVec2(24, 20);
	st.ItemSpacing = ImVec2(10, 10);
	st.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.06f, 0.16f, 0.94f);
	st.Colors[ImGuiCol_Header] = ImVec4(0.95f, 0.55f, 0.1f, 0.85f);
	st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.55f, 0.1f, 0.6f);
	st.Colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.65f, 0.2f, 1.0f);
	st.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.2f, 0.45f, 1.0f);
	st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.4f, 0.32f, 0.65f, 1.0f);
	st.Colors[ImGuiCol_Separator] = ImVec4(0.5f, 0.45f, 0.7f, 0.6f);

	ImGui_ImplSDL3_InitForVulkan(window);
	ImGui_ImplVulkan_InitInfo ii{};
	ii.Instance = vk.instance;
	ii.PhysicalDevice = vk.gpu;
	ii.Device = vk.device;
	ii.QueueFamily = vk.queue_family;
	ii.Queue = vk.queue;
	ii.DescriptorPoolSize = 8;   // font + trophy icons
	ii.MinImageCount = std::max(2u, vk.image_count);
	ii.ImageCount = std::max(2u, vk.image_count);
	ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	ii.UseDynamicRendering = true;
	ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format;
	ImGui_ImplVulkan_Init(&ii);
	// upload the font now, before the renderer starts sharing the queue
	ImGui_ImplVulkan_CreateFontsTexture();
	load_icons(vk);
	g_ready = true;
}

void shutdown() {
	if (!g_ready) return;
	free_icons();
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	g_ready = false;
}

void process_event(const SDL_Event& e) {
	if (!g_ready || g_screen == Screen::Closed) return;
	ImGui_ImplSDL3_ProcessEvent(&e);
	if (g_screen == Screen::Name) {
		if (e.type == SDL_EVENT_TEXT_INPUT)
			for (const char* c = e.text.text; *c; c++) type_char(*c);
		if (e.type == SDL_EVENT_KEY_DOWN) {
			if (e.key.key == SDLK_BACKSPACE && !g_name.empty()) g_name.pop_back();
			if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) finish_name();
			if (e.key.key == SDLK_ESCAPE) {
				SDL_StopTextInput(g_window);
				open_pick(g_owner);
			}
		}
	} else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
		if (g_screen == Screen::Trophies) open_pick(g_owner);
		else if (!g_join_mode) close_menu();   // also cancels Quit
	}
}

// Joins and leaves reported by the game thread; opens the next join menu when free.
void process_joins() {
	std::vector<std::pair<uint32_t, bool>> events;
	{
		std::lock_guard lock(g_join_lock);
		events.swap(g_join_events);
	}
	bool menu_on = g_screen != Screen::Closed;
	for (auto [port, joined] : events) {
		bool queued = std::find(g_waiting.begin(), g_waiting.end(), port) != g_waiting.end();
		bool asking = menu_on && g_join_mode && g_owner == port;
		if (joined) {
			// asked once per controller connection, not before every game
			if (queued || asking || (g_answered & (1u << port))) continue;
			g_waiting.push_back(port);
		} else {
			std::erase(g_waiting, port);
			g_bumped.erase(port);
			if (asking) close_menu();   // left before choosing: keeps what it had
		}
	}
	// a waiting controller that was unplugged is no longer waiting
	std::erase_if(g_waiting, [](uint32_t port) {
		if (input::raw_pad(port).connected) return false;
		g_bumped.erase(port);
		return true;
	});
	// joins during a match are asked once it is over
	if (g_screen == Screen::Closed && !g_releasing && !g_waiting.empty() && !profiles::in_match()) {
		uint32_t port = g_waiting.front();
		g_waiting.pop_front();
		g_join_mode = true;
		open_pick(port);
		LOG_INFO("overlay: %s %s, asking who is playing", slot_label(port).c_str(),
		         g_bumped.contains(port) ? "lost its profile" : "joined");
	}
}

// Which controllers the game may not see: all of them while a menu is open, the ones
// waiting for a join menu, and, after a menu closes, each one until it lets go (so a
// press made during the menu never reaches the game).
void sync_held() {
	for (uint32_t port = 0; port < input::MAX_PORTS; port++) {
		uint32_t bit = 1u << port;
		input::PadState s = input::raw_pad(port);
		bool pressing = s.connected && (s.digital1 || s.digital2);
		bool waiting = std::find(g_waiting.begin(), g_waiting.end(), port) != g_waiting.end();
		bool want = g_screen != Screen::Closed || waiting || ((g_held & bit) && pressing);
		if (want == bool(g_held & bit)) continue;
		input::set_captured(port, want);
		g_held ^= bit;
	}
	if (g_releasing && !(g_held & (1u << g_owner))) g_releasing = false;
}

void update() {
	process_joins();
	if (g_screen != Screen::Closed) controller_input();
	sync_held();
}

bool visible() {
	std::lock_guard lock(g_notice_lock);
	return g_screen != Screen::Closed || !g_notices.empty();
}

void render(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
	if (!g_ready || !visible()) return;
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	// the font is 40 px; scale everything to the window (40 px tall text at 2160 p)
	float scale = std::clamp(float(extent.height) / 1440.0f, 0.4f, 2.0f);
	ImGui::GetIO().FontGlobalScale = scale * 0.8f;
	ImGuiStyle& st = ImGui::GetStyle();
	st.WindowPadding = ImVec2(24 * scale, 20 * scale);
	st.ItemSpacing = ImVec2(10 * scale, 10 * scale);
	st.FramePadding = ImVec2(8 * scale, 6 * scale);

	draw_notices(scale);
	if (g_screen != Screen::Closed) {
		// dim the game behind the menu
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(float(extent.width), float(extent.height)),
		                                              IM_COL32(0, 0, 0, 110));
		ImGui::SetNextWindowPos(ImVec2(extent.width * 0.5f, extent.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(extent.width * 0.32f, 0), ImVec2(extent.width * 0.9f, extent.height * 0.9f));
		ImGui::Begin("##profiles", nullptr,
		             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
		                 ImGuiWindowFlags_NoNav);
		if (g_screen == Screen::Pick) draw_pick();
		else if (g_screen == Screen::Trophies) draw_trophies(extent, scale);
		else if (g_screen == Screen::Quit) draw_quit(scale);
		else draw_name(scale);
		ImGui::End();
	}
	ImGui::Render();

	VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	att.imageView = view;
	att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, extent};
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &att;
	vkCmdBeginRendering(cmd, &ri);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRendering(cmd);
}

void menu_button(uint32_t port) {
	if (port >= 4) return;   // profiles cover the four local players
	if (g_screen != Screen::Closed) {
		if (port == g_owner) close_menu();
		return;   // one menu at a time
	}
	open_pick(port);
}

void exit_combo(uint32_t port) { open_quit(port); }

void player_joined(uint32_t port) {
	std::lock_guard lock(g_join_lock);
	g_join_events.push_back({port, true});
}

void player_left(uint32_t port) {
	std::lock_guard lock(g_join_lock);
	g_join_events.push_back({port, false});
}

void controller_connected(uint32_t port) {
	if (port >= 4) return;
	g_answered &= ~(1u << port);   // maybe someone else: ask at the next join
	std::string name = profiles::assigned(port);
	notify(slot_label(port) + ": " + profile_label(name) + (name.empty() ? "  -  press PS / Guide (F1) to pick a profile" : ""));
}

void notify(const std::string& text) {
	LOG_INFO("overlay: %s", text.c_str());
	std::lock_guard lock(g_notice_lock);
	g_notices.push_back({text, SDL_GetTicks() + NOTICE_MS});
	if (g_notices.size() > 4) g_notices.erase(g_notices.begin());
}

} // namespace overlay
