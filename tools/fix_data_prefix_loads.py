"""Neutralise pointer-table words that XenonRecomp decoded as the first two instructions of a function.

Pattern (93 functions, locals mode): the 8 bytes before a real function entry are data (0x8213D280, 0x821Dxxxx:
a pointer table), decoded as
    lwz r16,-11648(r19)      ->  r16.u64 = PPC_LOAD_U32(r19.u32 + -11648);
    lwz r16,N(r16)           ->  r16.u64 = PPC_LOAD_U32(r16.u32 + N);
The real function starts 8 bytes later and has its own entry (callers use that one). In ctx mode the loads read a
mapped address and clobbered ctx.r16; in locals mode r19 is 0 (or whatever the live-in pass hands over), so the
first load reads guest 0xFFFFD280: unmapped on hardware -> data abort if this entry is ever called.
If r16 is not used anywhere else in the function, both loads are dead: replace them with a comment.
Functions where r16 is used later are reported and left alone.

Usage: python fix_data_prefix_loads.py <output_dir> [--dry-run]
"""
import glob, os, re, sys

FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)')
L1 = '\tr16.u64 = PPC_LOAD_U32(r19.u32 + -11648);'
L2_RE = re.compile(r'^\tr16\.u64 = PPC_LOAD_U32\(r16\.u32 \+ (-?\d+)\);$')
R16 = re.compile(r'(?<![\w.])r16(?!\w)')
MARK = '// LSW_DATA_PREFIX'


def main():
    d = sys.argv[1]; dry = '--dry-run' in sys.argv
    fixed, kept = [], []
    for p in sorted(glob.glob(os.path.join(d, 'ppc_recomp.*.cpp'))):
        lines = open(p, encoding='utf-8', newline='').read().split('\n')
        changed = False
        starts = [i for i, t in enumerate(lines) if FUNC_RE.match(t)]
        for k, s in enumerate(starts):
            e = starts[k + 1] if k + 1 < len(starts) else len(lines)
            for i in range(s, e - 2):
                if lines[i] != L1:
                    continue
                j = i + 1
                while j < e and lines[j].lstrip().startswith('//'):
                    j += 1
                m = L2_RE.match(lines[j]) if j < e else None
                if not m:
                    kept.append((p, FUNC_RE.match(lines[s]).group(1), 'second load missing'))
                    break
                others = [x for x in range(s + 1, e) if x not in (i, j) and R16.search(lines[x])
                          and not lines[x].lstrip().startswith('//') and 'PPCRegister r16{}' not in lines[x]
                          and 'g_lsw_live.r16' not in lines[x]]
                name = FUNC_RE.match(lines[s]).group(1)
                # a later full write of r16 (no r16 on the right-hand side) before any read also leaves the loads dead
                # (straight-line check: the first later mention, textually after the prefix, must be that write)
                if others and others[0] > j:
                    w = re.match(r'^\s*r16\.u64 = (.*);$', lines[others[0]])
                    if w and not R16.search(w.group(1)) and not any(R16.search(lines[x]) and not lines[x].lstrip().startswith('//')
                                                                     and 'g_lsw_live.r16' not in lines[x] for x in range(j + 1, others[0])):
                        others = []
                if others:
                    kept.append((p, name, f'r16 used at line {others[0] + 1}'))
                    break
                lines[i] = f'\t{MARK}: lwz r16,-11648(r19) is pointer-table data (0x8213D280) before the real entry: not executed'
                lines[j] = f'\t{MARK}: lwz r16,{m.group(1)}(r16) likewise (r16 unused in this function)'
                fixed.append((p, name)); changed = True
                break
        if changed and not dry:
            open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
    print(f'neutralised: {len(fixed)}   left alone: {len(kept)}')
    for p, n, why in kept:
        print(f'  kept {os.path.basename(p)} {n}: {why}')


if __name__ == '__main__':
    main()
