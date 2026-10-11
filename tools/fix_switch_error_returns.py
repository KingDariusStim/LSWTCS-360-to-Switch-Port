"""Turn XenonRecomp's `// ERROR` returns into the jumps they stand for.

XenonRecomp emits `// ERROR <target>` + `return;` wherever a branch or a switch case (`bctr` through a jump table)
leaves the function as it sized it. The target code usually exists (as a function of its own, or as a tiny
`li r3,K; blr` stub), but the jump to it is lost: the function just returns, with r3 still holding whatever it held.
Found 2026-10-10 through the Episode 2 hub disco ball switching itself on: the trigger network's class-2 object
state query sub_821CAE98 switches on the wanted state (0-7); every case was an ERROR return, so the query returned
the object pointer in r3 = "true" for any state, and the level's Force-block sequence counted as finished at hub
load. 53 functions have such switches (1322 cases), plus ~330 ERROR branches.

Rewrites, per ERROR site:
  - target is a recompiled function sub_T: store the callee's live-in registers that the caller holds as locals into
    g_lsw_live (as pass_livein_regs.py does at direct call sites, from livein_report.txt), then
    `sub_T(ctx, base); return;` (a tail call: what the original jump does);
  - target is `li r3,K; blr`: `ctx.r3.s64 = K; return;`;
  - target is `blr`: unchanged (the plain return is already right);
  - anything else: left alone and reported.

Usage: python fix_switch_error_returns.py <output_dir> <livein_report> <image.bin> [--range LO-HI] [--dry-run]
  --range limits the rewrite to sites inside functions whose address is in [LO, HI] (hex).
Marker: // LSW_SWITCH_FIX (sites already rewritten are skipped).
"""
import glob, os, re, struct, sys, collections

FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{')
DECL_RE = re.compile(r'^\t(PPC\w*Register) (\w+)\{\};$')
ERR_RE = re.compile(r'^(\s*)// ERROR(:)? (?:0x)?([0-9A-Fa-f]{8})\s*$')
MARK = '// LSW_SWITCH_FIX'
IMAGE_BASE = 0x82000000


def load_livein(path):
    live = {}
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^(sub_[0-9A-F]{8}) \S+: (.*)$', line.strip())
        if m:
            live[m.group(1)] = m.group(2).split()
    return live


def stub_of(img, t):
    o = t - IMAGE_BASE
    if not (0 <= o <= len(img) - 8):
        return None
    w1, w2 = struct.unpack('>II', img[o:o + 8])
    if w1 == 0x4E800020:
        return ('blr', None)
    if (w1 >> 16) == 0x3860 and w2 == 0x4E800020:
        return ('li', struct.unpack('>h', struct.pack('>H', w1 & 0xFFFF))[0])
    return None


def main():
    out_dir, report, image = sys.argv[1], sys.argv[2], sys.argv[3]
    dry = '--dry-run' in sys.argv
    lo, hi = 0, 0xFFFFFFFF
    if '--range' in sys.argv:
        a, b = sys.argv[sys.argv.index('--range') + 1].split('-')
        lo, hi = int(a, 16), int(b, 16)
    live = load_livein(report)
    img = open(image, 'rb').read()
    files = sorted(glob.glob(os.path.join(out_dir, 'ppc_recomp.*.cpp')))
    funcs = set()
    for p in files:
        for m in re.finditer(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{', open(p, encoding='utf-8').read(), re.M):
            funcs.add(int(m.group(1), 16))
    stats = collections.Counter()
    left = collections.Counter()
    for p in files:
        lines = open(p, encoding='utf-8', newline='').read().split('\n')
        changed = False
        cur, local = None, set()
        i = 0
        while i < len(lines):
            m = FUNC_RE.match(lines[i])
            if m:
                cur, local = int(m.group(1), 16), set()
            d = DECL_RE.match(lines[i])
            if d and cur is not None:
                local.add(d.group(2))
            e = ERR_RE.match(lines[i])
            if e and cur is not None and lo <= cur <= hi and i + 1 < len(lines) and lines[i + 1].strip() == 'return;' \
                    and MARK not in lines[i]:
                ind, t = e.group(1), int(e.group(3), 16)
                kind = 'case' if e.group(2) else 'branch'
                new = None
                if t in funcs:
                    name = 'sub_%08X' % t
                    stores = ['%sg_lsw_live.%s = %s;  %s' % (ind, r, r, MARK) for r in live.get(name, []) if r in local]
                    new = stores + ['%s%s(ctx, base);  %s' % (ind, name, MARK), '%sreturn;' % ind]
                    stats[kind + ' -> function'] += 1
                else:
                    s = stub_of(img, t)
                    if s and s[0] == 'li':
                        new = ['%sctx.r3.s64 = %d;  %s li r3,%d; blr at %08X' % (ind, s[1], MARK, s[1], t), '%sreturn;' % ind]
                        stats[kind + ' -> li r3,K; blr'] += 1
                    elif s and s[0] == 'blr':
                        stats[kind + ' -> blr (already right)'] += 1
                    else:
                        stats[kind + ' -> other (left)'] += 1
                        left[t] += 1
                if new:
                    lines[i] = lines[i] + '  ' + MARK
                    lines[i + 1:i + 2] = new
                    changed = True
                    i += len(new)
            i += 1
        if changed and not dry:
            open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    for k, v in sorted(stats.items()):
        print('%6d %s' % (v, k))
    if left:
        print('targets left alone:', len(left), ' '.join('%08X' % t for t, _ in left.most_common(20)))


if __name__ == '__main__':
    main()
