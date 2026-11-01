# Summarize profile.txt written by `bomberman --profile start:seconds`.
# usage: python -I tools/dbg/profile.py [profile.txt] [exe] [--top N]
# Prints, per thread, the functions with the most samples where the thread was
# (self) and the functions that were anywhere on the stack (inclusive).
import collections, os, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
top = 25
if '--top' in sys.argv:
    top = int(sys.argv[sys.argv.index('--top') + 1])
    args = [a for a in args if a != str(top)]
path = args[0] if args else 'profile.txt'
exe = args[1] if len(args) > 1 else 'build/clang-Release/bomberman.exe'
IMAGE_BASE = 0x140000000
SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
exe_name = os.path.basename(exe).lower()

threads = []   # (name, total, [(count, [frames])])
for line in open(path):
    parts = line.split()
    if parts[0] == 'thread':   # "thread <name, may contain spaces> <samples>"
        threads.append((' '.join(parts[1:-1]), int(parts[-1]), []))
    else:
        threads[-1][2].append((int(parts[0]), parts[1:]))

# symbolize every address in our executable in one llvm-symbolizer run
addrs = sorted({f for _, _, st in threads for _, fr in st for f in fr if f.lower().startswith(exe_name + '+')})
names = {}
if addrs:
    inp = '\n'.join(hex(IMAGE_BASE + int(a.split('+')[1], 16)) for a in addrs) + '\n'
    out = subprocess.run([SYMBOLIZER, '--obj=' + exe, '--no-inlines', '--functions=short', '--demangle'],
                         input=inp, capture_output=True, text=True).stdout
    blocks = out.strip().split('\n\n')
    for a, b in zip(addrs, blocks):
        fn = b.split('\n')[0]
        names[a] = fn if fn != '??' else a


def label(frame):
    if frame in names:
        return names[frame]
    return frame.split('+')[0]   # other modules: just the DLL name


for name, total, stacks in sorted(threads, key=lambda t: -t[1]):
    self_c, incl_c = collections.Counter(), collections.Counter()
    for count, frames in stacks:
        labels = [label(f) for f in frames]
        self_c[labels[0]] += count
        for l in set(labels):
            incl_c[l] += count
    print(f'== {name}: {total} samples')
    print('  self:')
    for fn, c in self_c.most_common(top):
        print(f'    {100 * c / total:5.1f}%  {fn}')
    print('  inclusive:')
    for fn, c in incl_c.most_common(top):
        print(f'    {100 * c / total:5.1f}%  {fn}')
    print()
