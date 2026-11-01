"""Call hooks: game call sites routed through runtime functions.

A `bl` at one of these addresses calls the named runtime function instead of its
target; the hook gets the original target as `original` and decides when (and
whether) to call it. Hooks are declared in the generated ppu_funcs.h as
`void name(PPUContext& ctx, PPUFunc original);` and defined in the runtime.
"""

CALL_HOOKS = {
    # Player Select slot label: formats IDS_PLAYER_NUMBER ("Player %1") into a
    # 20-character buffer (r3) with the player number (r6); the runtime puts the
    # profile name there instead.
    0x1295C0: "hook_player_label",
    # BBVNet::GetPlayerName(net, slot) (0x8D6F8) from the player slot manager: the
    # name a slot carries into the match HUD and results ("Player N" offline).
    0xDD520: "hook_slot_name",
    0xDDB50: "hook_slot_name",
    # Custom Arenas (runtime/src/profiles/custom_arenas.cpp). sub_0010A800 builds the
    # Arena Select list; the saved custom arenas are appended after Random.
    0x10A9FC: "hook_arena_list",
    0x10BC04: "hook_arena_list",
    # sub_000457A8(framework, user, slot, out): a custom arena's saved slot (Arena
    # Select, arena ids 17-19); answered from the whole-game slots.
    0x109FA4: "hook_custom_slot",
    0x10A2C8: "hook_custom_slot",
    # ...whose power-up word is then unpacked into the match config (sub_000E41A8),
    # leaving each switch as its bit value; the checkboxes want 0 / 1.
    0x109FE0: "hook_unpack_powerups",
    0x10A304: "hook_unpack_powerups",
    # Power-Up Settings -> Save Settings: NavigateTo("SaveMatchScreen.xui").
    0x130770: "hook_save_settings",
    # Power-Up Settings init hides and disables the Save Settings button
    # (sub_0012D5C0 / sub_0012DC58 with 0); the hook shows and enables it.
    0x1314E4: "hook_show_save_button",
    0x1314F4: "hook_show_save_button",
    # The XUI class registrations (sub_000E57D8): after CPowerupSettingsScreen's,
    # CSaveMatchScreen (runtime/src/profiles/save_match_screen.cpp) is registered.
    0xE5F00: "hook_register_classes",
    # Match lock (runtime/src/profiles/match_lock.cpp): the registrations of the match
    # screens, from the arena's loading screen to the results; their create / destroy
    # are counted so profiles stay put while a match is on.
    0xE5A8C: "hook_match_screen_class",   # CGameLoadingScreen
    0xE5BBC: "hook_match_screen_class",   # CInGameUI
    0xE5E1C: "hook_match_screen_class",   # CPauseScreen
    0xE5F9C: "hook_match_screen_class",   # CQuickFinishScreen
    0xE60D0: "hook_match_screen_class",   # CResultsScreenLive
    0xE611C: "hook_match_screen_class",   # CResultsScreenLocal
    0xE62E8: "hook_match_screen_class",   # CVictoryScreen
    # The "not connected to PSN" popup on entering Player Select (sub_000EB670, Ok
    # only, result unused); skipped (runtime/src/hle/psn_notice.cpp).
    0x129484: "hook_psn_notice",
    # Wardrobe pickup retry (runtime/src/profiles/wardrobe.cpp, settings.ini [game]
    # aggressive_costume_spawns): the spawn's random tile pick (sub_00031388) and the
    # pickup's constructor (sub_0007BAB8) in sub_00055400.
    0x55A98: "hook_wardrobe_pick",
    0x55B6C: "hook_wardrobe_placed",
    # Match HUD position (runtime/src/hle/hud_offset.cpp, settings.ini hud_raise): the
    # quad submits (sub_0009EEA8) of the bar across the top of a match: head shadow,
    # head, head marker, digits (wins and time), clock box.
    0x660DC: "hook_hud_quad",
    0x661BC: "hook_hud_quad",
    0x66340: "hook_hud_quad",
    0x6434C: "hook_hud_quad",
    0x6656C: "hook_hud_quad",
    # CPU players (runtime/src/hle/bot.cpp, settings.ini cpu_ai): match setup's call of the
    # CPU player constructor sub_00058A20(obj, player, difficulty); with cpu_ai = smart the
    # player is made a human-class player on a free pad port that the bot drives.
    0x5236C: "hook_cpu_player",
}
