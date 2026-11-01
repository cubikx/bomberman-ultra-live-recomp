# For one thread in profile.txt: group samples by the innermost frames that are
# in our executable (the "where in our code" of waits inside system DLLs).
# usage: python -I tools/dbg/profile_callers.py <thread name> [depth] [profile.txt] [exe]
import collections, subprocess, sys
name = sys.argv[1]
depth = int(sys.argv[2]) if len(sys.argv) > 2 else 3
path = sys.argv[3] if len(sys.argv) > 3 else 'profile.txt'
exe = sys.argv[4] if len(sys.argv) > 4 else 'build/clang-Release/bomberman.exe'
stacks, cur, total = [], None, 0
for line in open(path):
    p = line.split()
    if p[0] == 'thread':
        cur = ' '.join(p[1:-1]); continue
    if cur == name:
        stacks.append((int(p[0]), [f for f in p[1:] if f.lower().startswith('bomberman.exe+')]))
addrs = sorted({f for _, fr in stacks for f in fr})
out = subprocess.run([r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe', '--obj=' + exe, '--no-inlines', '--functions=short'],
                     input=''.join(hex(0x140000000 + int(a.split('+')[1], 16)) + '\n' for a in addrs),
                     capture_output=True, text=True).stdout.strip().split('\n\n')
sym = {a: b.split('\n')[0] for a, b in zip(addrs, out)}
c = collections.Counter()
for n, fr in stacks:
    total += n
    c[' <- '.join(sym[f] for f in fr[:depth])] += n
for k, n in c.most_common(25):
    print(f'{100 * n / total:5.1f}%  {k}')
