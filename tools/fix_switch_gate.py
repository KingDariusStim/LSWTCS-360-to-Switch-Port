"""Wrap every ERROR-return repair (LSW_SWITCH_FIX / LSW_SWITCH_MID sites) in `if (lsw_fixon(<function>)) { ... }` so
LSWTCS_FIXOFF can switch repairs off per function at run time (lsw_fixgate.h). Run after fix_switch_error_returns.py
and fix_switch_midjumps.py. Idempotent (marker // LSW_FIXGATE). Usage: python fix_switch_gate.py <output_dir>"""
import glob, os, re, sys

HDR_RE = re.compile(r'^(?:PPC_FUNC_IMPL\(__imp__sub_|PPC_FUNC\(lsw_entry_)([0-9A-F]{8})\)')
ERR_RE = re.compile(r'^(\s*)// ERROR:? (?:0x)?[0-9A-Fa-f]{8}.*// LSW_SWITCH_(?:FIX|MID)')
total = 0
for p in sorted(glob.glob(os.path.join(sys.argv[1], 'ppc_recomp.*.cpp'))):
    lines = open(p, encoding='utf-8', newline='').read().split('\n')
    if not any('LSW_SWITCH_' in l for l in lines) or any('LSW_FIXGATE' in l for l in lines):
        continue
    out, func, i = [], 0, 0
    while i < len(lines):
        m = HDR_RE.match(lines[i])
        if m:
            func = int(m.group(1), 16)
        e = ERR_RE.match(lines[i])
        if e:
            ind = e.group(1)
            j = i + 1
            while lines[j].strip() != 'return;':
                j += 1
            out.append(lines[i])
            out.append('%sif (lsw_fixon(0x%08X)) {  // LSW_FIXGATE' % (ind, func))
            out += ['\t' + l for l in lines[i + 1:j]]
            out.append('\t%sreturn;' % ind)
            out.append('%s}' % ind)
            out.append(lines[j])
            total += 1
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    text = '\n'.join(out).replace('#include "ppc_recomp_shared.h"\n', '#include "ppc_recomp_shared.h"\n#include "lsw_fixgate.h"\n', 1)
    open(p, 'w', encoding='utf-8', newline='').write(text)
print('sites gated:', total)
