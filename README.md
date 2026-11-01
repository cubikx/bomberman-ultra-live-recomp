# Bomberman ULTRA for Windows

A native Windows port of **Bomberman ULTRA** (PS3, PlayStation Network release `NPUB30051`,
version 01.04). The PS3 game code is recompiled to C++ and runs with its own graphics
(Vulkan), audio, input and save system. No emulator is involved.

The project contains **no game files**. You need your own copy of the PS3 game, and
optionally your own copy of the Xbox 360 game *Bomberman LIVE* for the "Bomberman LIVE"
look (see [Optional: Bomberman LIVE](#optional-bomberman-live)).

---

## What you need

**To play**
- Windows 10 or 11, 64-bit
- A graphics card with Vulkan support (any NVIDIA / AMD / Intel GPU from the last ~10 years)
- A controller (Xbox, PlayStation or other SDL-supported pad) or the keyboard

**To build** (install these once)
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with the
  **Desktop development with C++** workload
- [LLVM](https://github.com/llvm/llvm-project/releases) (provides `clang-cl`; the code needs clang)
- [CMake](https://cmake.org/download/) 3.24 or newer
- [Ninja](https://github.com/ninja-build/ninja/releases) (on `PATH`)
- [Python](https://www.python.org/downloads/) 3.10 or newer (on `PATH` as `python`)
- [Git](https://git-scm.com/download/win) (CMake downloads SDL3, Vulkan headers, Dear ImGui and stb)

The build needs an internet connection the first time, for those downloads.

---

## Setting up for the first time

All paths below are relative to this project folder (the one containing `build.cmd`).

### Step 1: Put the PS3 game files in `NPUB30051\`

1. Take the game's `NPUB30051` folder from your PS3 (or from your installed PSN copy). If you
   have it as a `.zip` / `.7z`, **extract it**. The game cannot read it from an archive.
2. Place it in this project folder, so you get:

   ```
   NPUB30051\
     ICON0.PNG
     PARAM.SFO
     TROPDIR\
     USRDIR\
       EBOOT.BIN
       media.zip
       Bman.bnk
   ```

3. **Decrypt the executable.** `USRDIR\EBOOT.BIN` is encrypted; the port needs the decrypted
   `EBOOT.elf` next to it. With [RPCS3](https://rpcs3.net/) and your license (`.rap` file)
   installed: **Utilities → Decrypt PS3 Binaries**, select `USRDIR\EBOOT.BIN`. RPCS3 writes
   `EBOOT.elf`; make sure it ends up as `NPUB30051\USRDIR\EBOOT.elf`.

   When done, `NPUB30051\USRDIR\EBOOT.elf` must exist. The game must be **version 01.04**.

### Step 2: Recompile the game code

Open a terminal in this project folder and run these three commands (a few minutes in total):

```bash
python -I recomp/recompile.py NPUB30051/USRDIR/EBOOT.elf game/generated
```

```bash
python -I recomp/spu_recompile.py NPUB30051/USRDIR/EBOOT.elf 0x1A1F00 42240 0x1000 0x1010 job_1a1f00 game/generated/spu
```

```bash
python -I recomp/spu_recompile.py NPUB30051/USRDIR/EBOOT.elf 0x18D280 0x14370 0x3000 0x3080 multistream game/generated/spu
```

This writes C++ files to `game\generated\`. Only re-run them if the recompiler changes.

### Step 3: Build

```bash
cmd /c ".\build.cmd Release"
```

The game ends up in `build\clang-Release\bomberman.exe`. The build also links, in that folder:

| In `build\clang-Release\` | Points to | Holds |
|---|---|---|
| `game\` | `NPUB30051\` | the PS3 game files |
| `user\` | `user\` | your settings, saves, trophies (created on first start) |
| `live\` | `live\` | the optional Bomberman LIVE files |

### Step 4: Play

```bash
build\clang-Release\bomberman.exe
```

On the first start the game creates `user\`:

```
user\
  settings\settings.ini   all options, with explanations (edit with any text editor)
  saves\                  save data
  trophies\               unlocked trophies
  cache\                  files the game rebuilds by itself (safe to delete)
```

Deleting any one of these folders resets just that part; the game starts cleanly without it.

**Sharper picture:** the game renders at a multiple of the PS3's 1280x720, chosen to suit your
display (`render_scale = auto` in `settings.ini`). Set `render_scale` to `1`
to `4` to choose it yourself; `1` is the PS3 look, higher is sharper and needs a faster
graphics card. Restart the game after changing it.

**Costume pickups:** the wardrobe icon (it unlocks a costume piece) appears about a minute
into local matches of 2 minutes or more. The original game skips it whenever its spot is
still covered by a block; this port keeps trying other spots
(`aggressive_costume_spawns = on`). Set it to `off` for the original behaviour.

**Status bar:** in a match, the bar with the player heads, wins and clock sits higher than
in the original game, so it no longer covers the top of the arena (`hud_raise = 36`, in
pixels of 720). Set it to `0` for the original position.

---

## Controls

**Controllers** work as on the PS3. Xbox pads: A = Cross, B = Circle, X = Square,
Y = Triangle, LB/RB = L1/R1, LT/RT = L2/R2, Back = Select.

**Keyboard** (player 1; by default only while no controller is connected, see `keyboard` in
`settings.ini`):

| Key | Button | | Key | Button |
|---|---|---|---|---|
| Arrow keys | D-pad | | Enter | Start |
| Z or Space | Cross | | Backspace | Select |
| X or Esc | Circle | | Q / W | L1 / R1 |
| A | Square | | 1 / 2 | L2 / R2 |
| S | Triangle | | | |

**Other keys and buttons**

| | |
|---|---|
| F1, or the PS / Guide button | Profile menu: pick or create who is playing, see trophies |
| F11, or Alt+Enter | Full screen on / off |
| Hold L2 + R2 + Select + Start for 1 s | "Quit game?" (LT + RT + Back + Start on Xbox pads) |

---

## Optional: Bomberman LIVE

The PS3 game is the PS3 version of the Xbox 360 game *Bomberman LIVE*. With your own 360
copy, the port can look and sound like the 360 version: its logo and title screen, the spoken
"Bomberman Live!", A/B/X/Y button icons, the 360 controller picture and the 360 wording. The
game content (14 arenas, all modes and costumes, up to 7 players on one PC) stays that of the
PS3 version.

### Step A: Put the Xbox 360 game files in `xbox360\`

1. From your 360 copy, take `default.xex` and the `newmedia` folder. Extract them if they are
   in an archive.
2. **Decrypt the executable** with xextool (a common Xbox 360 homebrew tool):
   `xextool -e u -o default_dec.xex default.xex`
3. Place them in this project folder, so you get:

   ```
   xbox360\
     default_dec.xex
     newmedia\
       XUIResource2.xzp
       music.xwb
   ```

   (Other files in the folder are ignored.)

### Step B: Get vgmstream (for the voice)

The 360 voice is in Xbox audio format (XMA); [vgmstream](https://github.com/vgmstream/vgmstream/releases)
decodes it. Download `vgmstream-win64.zip` and extract it to `tools\ref\vgmstream\`, so that
`tools\ref\vgmstream\vgmstream-cli.exe` exists.

### Step C: Make the Bomberman LIVE files

```bash
python -I tools/media/make_live.py
```

This converts the 360 files and writes 22 files to `live\` (pictures, text in 7 languages,
and the voice). It does not change your game files. If `live\` is ever lost, run the command
again: it makes exactly the same files from the originals.

Options, if your files are elsewhere: `python -I tools/media/make_live.py <360 folder>
--ps3 <PS3 game folder> --out <folder> --vgmstream <vgmstream-cli.exe>`, or `--no-voice` to
make everything except the voice.

### Step D: Switch it on

In `user\settings\settings.ini`, set:

```ini
presentation = live
```

Start the game. Set it back to `ultra` for the PS3 look. The first start after switching
takes a few seconds longer while the game rebuilds its files in `user\cache\`.

---

## Making a separate folder to play from

The build folder works for playing, but it is full of build files and its `game`, `user` and
`live` are links back into the project. For a clean game folder (to play from, or to copy to
another PC), make a new folder, for example `bomberman\`, holding only:

```
bomberman\
  bomberman.exe   from build\clang-Release\
  game\           copy of NPUB30051\ (EBOOT.BIN and Thumbs.db are not needed)
  live\           optional: copy of live\
  user\           optional: copy of user\ to keep your settings, saves and trophies
                  (leave out user\cache\; it is rebuilt). Created on first start if missing.
```

Copy real folders, not the links from the build folder. After a new build, copying
`bomberman.exe` again is enough. Other locations can be given with `--game <folder>`,
`--user <folder>` and `--live <folder>`.

---

## Troubleshooting

- **"Game files not found"**: `game\USRDIR\EBOOT.elf` is missing next to the exe (or
  `NPUB30051\USRDIR\EBOOT.elf` in this project). See [Step 1](#step-1-put-the-ps3-game-files-in-npub30051).
- **Build stops with "Run the recompiler first"**: do [Step 2](#step-2-recompile-the-game-code).
- **Build stops with "Build with clang / clang-cl"**: install LLVM; `build.cmd` looks for
  `clang-cl` on `PATH` and in `C:\Program Files\LLVM\bin`.
- **Still Bomberman ULTRA after setting `presentation = live`**: the `live\` folder is empty or
  missing. Run [Step C](#step-c-make-the-bomberman-live-files); the game's log says
  `media: presentation = live, but ... has no files` in this case.
- **`make_live.py` says "vgmstream-cli not found"**: do [Step B](#step-b-get-vgmstream-for-the-voice),
  or add `--no-voice`.
- **Slow or stuttering on an older graphics card**: set `render_scale = 1` in
  `user\settings\settings.ini`.
- **Settings got messed up**: delete `user\settings\settings.ini`; it is recreated with the
  defaults on the next start.
