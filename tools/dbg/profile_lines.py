# Source-line view of one thread in profile.txt: where the samples land (leaf frame,
# inlined calls included), optionally only samples whose stack contains a function.
# usage: python -I tools/dbg/profile_lines.py <thread> [function substring] [--top N]
import collections, os, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
top = 40
if '--top' in sys.argv:
    top = int(sys.argv[sys.argv.index('--top') + 1])
    args = [a for a in args if a != str(top)]
thread = args[0]
within = args[1] if len(args) > 1 else None
path, exe = 'profile.txt', 'build/clang-Release/bomberman.exe'
IMAGE_BASE = 0x140000000
SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
exe_name = os.path.basename(exe).lower()

stacks, total, cur = [], 0, None
for line in open(path):
    parts = line.split()
    if parts[0] == 'thread':
        cur = ' '.join(parts[1:-1])
        if cur == thread:
            total = int(parts[-1])
    elif cur == thread:
        stacks.append((int(parts[0]), parts[1:]))


def symbolize(addrs, inlines):
    if not addrs:
        return {}
    inp = '\n'.join(hex(IMAGE_BASE + int(a.split('+')[1], 16)) for a in addrs) + '\n'
    flags = ['--inlines' if inlines else '--no-inlines', '--functions=short', '--demangle']
    out = subprocess.run([SYMBOLIZER, '--obj=' + exe] + flags, input=inp, capture_output=True, text=True).stdout
    res = {}
    for a, b in zip(addrs, out.strip().split('\n\n')):
        lines = b.split('\n')
        res[a] = [(lines[i], lines[i + 1]) for i in range(0, len(lines) - 1, 2)]   # innermost first
    return res


exe_frames = sorted({f for _, fr in stacks for f in fr if f.lower().startswith(exe_name + '+')})
funcs = symbolize(exe_frames, False)
leaves = symbolize(sorted({fr[0] for _, fr in stacks if fr[0] in funcs}), True)

by_line, n = collections.Counter(), 0
for count, frames in stacks:
    if within and not any(within in funcs.get(f, [('', '')])[0][0] for f in frames):
        continue
    n += count
    leaf = frames[0]
    if leaf in leaves:
        fn, loc = leaves[leaf][0]
        by_line[f'{fn}  {os.path.basename(loc.split(":")[0])}:{loc.split(":")[1] if ":" in loc else "?"}'] += count
    else:
        by_line[leaf.split('+')[0]] += count
print(f'{thread}: {total} samples, {n} {"within " + within if within else ""}')
for k, c in by_line.most_common(top):
    print(f'  {100 * c / total:5.1f}%  {k}')
