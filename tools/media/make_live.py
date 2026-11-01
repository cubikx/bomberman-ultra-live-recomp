# Build the "Bomberman LIVE" presentation files (the live/ folder) from the user's own
# Xbox 360 copy and PS3 game files. The output is the same bytes on every run, so a lost
# live/ folder is remade exactly from the originals.
# usage: python -I make_live.py [<360 game folder>] [--ps3 <PS3 game folder>] [--out <dir>]
#                               [--vgmstream <vgmstream-cli>] [--no-voice]
#   <360 game folder>  default_dec.xex (decrypted) and newmedia/ (XUIResource2.xzp, music.xwb);
#           default: xbox360/ next to the PS3 game folder
#   --ps3   the PS3 game folder (USRDIR/media.zip, USRDIR/Bman.bnk); default: game/ beside
#           bomberman.exe when run from an install's tools/media/, else this repo's NPUB30051/
#   --out   default: live/ next to that game folder (where bomberman.exe looks for it)
#   --vgmstream  decodes the 360's XMA audio for the voice; default: $VGMSTREAM,
#           tools/ref/vgmstream/vgmstream-cli.exe, then PATH. Get it from
#           https://github.com/vgmstream/vgmstream/releases (vgmstream-win64.zip).
#   --no-voice   skip the voice (Bman.bnk.patch) instead of failing without vgmstream
# Writes media.zip replacements (same names as the PS3 entries) that the runtime swaps
# in when settings.ini has `presentation = live` (runtime/src/hle/media_patch.cpp):
#   - textures: 360 logo, title screen, A/B/X/Y and LB/RB/LT/RT prompts, controller
#     diagram, Xbox Live Vision help picture, converted to the PS3 .texture.ps3 format
#   - strings_<lang>.xus: the PS3 tables with the product name and the console / PSN
#     wording taken from the 360 tables. The PS3 content (14 arenas, 7 local players,
#     Friendly Fire, ...) is kept, so whole 360 tables are not used.
#   - Bman.bnk.patch: the title screen's spoken "Bomberman Live!" (360 wave bank cue
#     bombermanLive) in place of the PS3's "Bomberman Ultra!", which the PS3 bank still
#     calls bombermanLive. Applied by the runtime to a copy of USRDIR/Bman.bnk.
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pstex
import vag

# Bman.bnk (Sony BNK v3): data section offset at file 0x10. Wave entry of bombermanLive
# (stream 8): u32 data offset at 0xBE0, u32 size at 0xBE4, 44050 Hz mono PS-ADPCM.
BANK_DATA_AT, VOICE_ENTRY, VOICE_OFFSET, VOICE_SIZE, VOICE_RATE = 0x10, 0xBE0, 0x23CA0, 0xBD10, 44050
XWB_VOICE = 72   # music.xwb stream (1-based) named bombermanLive

# PS3 texture name -> path in the 360 packages ('xex:' = the XUIZ in default_dec.xex)
TEXTURES = {
	'A': 'xex:resource/A.png', 'B': 'xex:resource/B.png',
	'X': 'xex:resource/X.png', 'Y': 'xex:resource/Y.png',
	'LT': 'xex:resource/LT.png', 'RT': 'xex:resource/RT.png',
	'LB_N': 'xex:resource/fe_images/LB_N.png', 'LB_HL': 'xex:resource/fe_images/LB_HL.png',
	'RB_N': 'xex:resource/fe_images/RB_N.png', 'RB_HL': 'xex:resource/fe_images/RB_HL.png',
	'logo': 'xex:resource/fe_images/logo.png',
	'titleScreen': 'xex:resource/titleScreen.png',
	'controller': 'xex:resource/fe_images/controller.png',
	'p9': 'xzp:help_images/p9.png',   # USB camera -> Xbox Live Vision (the help scene's frame is 360-sized)
}

# Keys whose 360 text replaces the PS3 text: names of the console, pad, camera, network
# and gamer ID, where the rest of the text is the same in both versions.
FROM_360 = [
	'IDS_CONTROLS_TOOLTIP', 'IDS_HELP_TOOLTIP',
	'IDS_POPUP_CONTROLLER_DISCONNECT_TITLE', 'IDS_POPUP_CONTROLLER_DISCONNECT_DESC',
	'IDS_POPUP_SAVE_CHARACTER_DESC', 'IDS_POPUP_SAVE_SETTINGS_DESC', 'IDS_POPUP_SAVE_SETTINGS_TITLE',
	'IDS_GAMERTAG', 'IDS_CONTROL_TOGGLE_GAMERTAG',
	'IDS_CAMERA_SETTING', 'IDS_CAMERA_SETTING_TOOLTIP', 'IDS_TITLE_CAMERA_SETTING_MENU',
	'IDS_HELP_TITLE_08', 'IDS_HELP_TEXT_010',
	'IDS_CAMERA_STATUS_DISABLED', 'IDS_CAMERA_STATUS_DISCONNECTED', 'IDS_CAMERA_STATUS_ERROR',
	'IDS_CAMERA_STATUS_ERROR_FULLSPEED', 'IDS_CAMERA_STATUS_INITIALIZED',
	'IDS_LEADERBOARD_XBOX_LIVE_PLAYER_WEEKLY', 'IDS_LEADERBOARD_XBOX_LIVE_PLAYER_TOURNAMENT_WEEKLY',
	'IDS_LEADERBOARD_XBOX_LIVE_RANKED', 'IDS_LEADERBOARD_XBOX_LIVE_RANKED_WEEKLY',
	'IDS_LEADERBOARD_XBOX_LIVE_RANKED_TOURNAMENT_WEEKLY',
	'IDS_NOT_ALL_PLAYERS_JOINED', 'IDS_POPUP_LIVE_REQUIRED', 'IDS_POPUP_NETWORK_DISCONNECT_DESC',
	'IDS_POPUP_XBOX_LIVE_NOT_CONNECTED', 'IDS_XBOX_LIVE_RANKED_TOOLTIP', 'IDS_UPSELL_NO_PRIVILEGE_DESC',
	'IDS_RETURN_TO_ARCADE', 'IDS_RETURN_TO_ARCADE_TOOLTIP',
]
# Everywhere else the product name is replaced in place, except the save data title
# (kept so existing saves look the same).
KEEP_NAME = {'IDS_SAVE_NAME_TITLE'}
LANGS = ['en', 'de', 'es', 'fr', 'it', 'jp', 'kr']


def find_ps3():
	root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
	for d in ('game', 'NPUB30051'):
		if os.path.exists(os.path.join(root, d, 'USRDIR', 'media.zip')):
			return os.path.join(root, d)
	return None


def find_vgmstream(given):
	here = os.path.dirname(os.path.abspath(__file__))
	for exe in (given, os.environ.get('VGMSTREAM'), os.path.join(here, '..', 'ref', 'vgmstream', 'vgmstream-cli.exe'),
	            shutil.which('vgmstream-cli')):
		if exe and os.path.exists(exe):
			return exe
	return None


def main():
	ap = argparse.ArgumentParser(description='Make the Bomberman LIVE files (live/) from the 360 and PS3 originals.')
	ap.add_argument('x360', nargs='?', help='360 game folder (default_dec.xex, newmedia/); default: xbox360/ next to the PS3 game folder')
	ap.add_argument('--ps3', help='PS3 game folder (USRDIR/media.zip, USRDIR/Bman.bnk)')
	ap.add_argument('--out', help='output folder (default: live/ next to the PS3 game folder)')
	ap.add_argument('--vgmstream', help='vgmstream-cli executable')
	ap.add_argument('--no-voice', action='store_true', help='skip the voice')
	a = ap.parse_args()
	ps3 = a.ps3 or find_ps3()
	if not ps3 or not os.path.exists(os.path.join(ps3, 'USRDIR', 'media.zip')):
		sys.exit('PS3 game folder not found: pass --ps3 <folder containing USRDIR/media.zip>')
	src = a.x360 or os.path.join(os.path.dirname(os.path.abspath(ps3)), 'xbox360')
	out = a.out or os.path.join(os.path.dirname(os.path.abspath(ps3)), 'live')
	vgm = None
	if not a.no_voice:
		vgm = find_vgmstream(a.vgmstream)
		if not vgm:
			sys.exit('vgmstream-cli not found (needed for the voice): pass --vgmstream <exe>, or --no-voice.\n'
			         'Download vgmstream-win64.zip from https://github.com/vgmstream/vgmstream/releases')
	for f in ('default_dec.xex', 'newmedia/XUIResource2.xzp', 'newmedia/music.xwb'):
		if not os.path.exists(os.path.join(src, f)):
			sys.exit(f'360 file missing: {os.path.join(src, f)}')
	print(f'360: {src}\nPS3: {ps3}\nout: {out}')
	media = zipfile.ZipFile(os.path.join(ps3, 'USRDIR', 'media.zip'))

	xex = open(os.path.join(src, 'default_dec.xex'), 'rb').read()
	at = xex.find(b'XUIZ')
	if at < 0:
		sys.exit('default_dec.xex has no XUIZ package (is it decrypted?)')
	pkgs = {'xex': pstex.xuiz_files(xex, at),
	        'xzp': pstex.xuiz_files(open(os.path.join(src, 'newmedia', 'XUIResource2.xzp'), 'rb').read())}
	os.makedirs(out, exist_ok=True)

	for name, where in TEXTURES.items():
		pkg, path = where.split(':', 1)
		w, h, rgba = pstex.read_png(pkgs[pkg][path])
		data = pstex.from_rgba(media.read(name + '.texture.ps3'), w, h, rgba)
		open(os.path.join(out, name + '.texture.ps3'), 'wb').write(data)
		print(f'{name}.texture.ps3  {w}x{h}')

	for lang in LANGS:
		table_ps3 = pstex.read_xus(media.read(f'strings_{lang}.xus'))
		x360 = dict(pstex.read_xus(pkgs['xex'][f'resource/languages/strings_{lang}.xus']))
		taken = renamed = 0
		table = []
		for k, v in table_ps3:
			if k in FROM_360 and k in x360:
				v = x360[k]; taken += 1
			elif k not in KEEP_NAME and 'Bomberman ULTRA' in v:
				v = v.replace('Bomberman ULTRA', 'Bomberman LIVE'); renamed += 1
			table.append((k, v))
		open(os.path.join(out, f'strings_{lang}.xus'), 'wb').write(pstex.write_xus(table))
		print(f'strings_{lang}.xus  {taken} from the 360, {renamed} renamed')

	if vgm:
		voice(src, out, os.path.join(ps3, 'USRDIR', 'Bman.bnk'), vgm)
	else:
		print('voice skipped (--no-voice)')


def read_wav(path):
	w = wave.open(path)
	assert w.getnchannels() == 1 and w.getsampwidth() == 2, path
	return w.getframerate(), struct.unpack(f'<{w.getnframes()}h', w.readframes(w.getnframes()))


def voice(src, out, bank, exe):
	with open(bank, 'rb') as f:
		head = f.read(VOICE_ENTRY + 8)
		data_at, = struct.unpack('>I', head[BANK_DATA_AT:BANK_DATA_AT + 4])
		if struct.unpack('>II', head[VOICE_ENTRY:VOICE_ENTRY + 8]) != (VOICE_OFFSET, VOICE_SIZE):
			sys.exit('Bman.bnk is not the expected version (bombermanLive entry differs)')
		f.seek(data_at + VOICE_OFFSET)
		ultra = vag.decode(f.read(VOICE_SIZE))
	with tempfile.TemporaryDirectory() as tmp:
		wav = os.path.join(tmp, 'live.wav')
		subprocess.run([exe, '-s', str(XWB_VOICE), '-o', wav, os.path.join(src, 'newmedia', 'music.xwb')],
		               check=True, stdout=subprocess.DEVNULL)
		rate, live = read_wav(wav)
	pcm = vag.resample(live, rate, VOICE_RATE)
	gain = max(abs(s) for s in ultra) / max(abs(s) for s in pcm)   # as loud as the PS3 call
	pcm = [max(-32768, min(32767, round(s * gain))) for s in pcm]
	frames = bytearray(16) + vag.encode(pcm)
	frames[-15] = 1                           # last frame of the sound
	frames += bytes((0, 7)) + bytes(14)       # stop frame, counted in the size as in the PS3 bank
	size = len(frames)
	if len(frames) > VOICE_SIZE + 16:         # the slot runs to the next sound's data
		sys.exit(f'voice is {len(frames)} bytes, the slot holds {VOICE_SIZE + 16}')
	frames += bytes(VOICE_SIZE + 16 - len(frames))
	# patch records: u32 file offset, u32 length, bytes (big-endian)
	patch = struct.pack('>II', data_at + VOICE_OFFSET, len(frames)) + frames
	patch += struct.pack('>III', VOICE_ENTRY + 4, 4, size)
	open(os.path.join(out, 'Bman.bnk.patch'), 'wb').write(patch)
	print(f'Bman.bnk.patch  bombermanLive {len(pcm) / VOICE_RATE:.2f}s (was {len(ultra) / VOICE_RATE:.2f}s), gain {gain:.2f}')


if __name__ == '__main__':
	main()
