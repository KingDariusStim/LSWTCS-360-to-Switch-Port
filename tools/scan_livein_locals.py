"""Scan recompiled output (locals mode) for functions that read a register local before writing it.

In locals mode every function declares its own zero-initialised `PPCRegister rN{}` etc., so a function
that expects a value in a non-argument/non-volatile register from its caller (MSVC SEH __finally /
__except funclets: called with r12 = parent frame and the parent's r14-r31 live) silently sees 0.

Usage: python scan_livein_locals.py <output_dir> [--detail]
Heuristic: first textual mention of the local in the function body (after its declaration) is a read
(it appears on a line that is not `rN.xxx = ...` with rN absent from the right-hand side).
"""
import re, sys, os, glob, collections

FUNC_RE = re.compile(r'^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)')
DECL_RE = re.compile(r'^\tPPC(?:CR|XER)?Register (\w+)\{\};|^\tPPCRegister (\w+)\{\};')

def scan(path):
    out = []
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        if not m:
            i += 1; continue
        name = m.group(1); j = i + 1; locs = []
        body = []
        while j < len(lines) and not lines[j].startswith('PPC_FUNC_IMPL') and not lines[j].startswith('__attribute__'):
            d = DECL_RE.match(lines[j])
            if d: locs.append(d.group(1) or d.group(2))
            elif not lines[j].lstrip().startswith('//'): body.append((j + 1, lines[j]))
            j += 1
        live = []
        for r in locs:
            if r in ('xer',) or r.startswith('cr'):   # flags: read-before-write is normal compare temp use? keep only GPR/FPR/VR
                pass
            pat = re.compile(r'(?<![\w.])' + re.escape(r) + r'\.')
            for ln, t in body:
                if pat.search(t):
                    lhs = re.match(r'^\s*' + re.escape(r) + r'\.\w+(?:\[\d+\])? = (.*);', t)
                    if lhs and not pat.search(lhs.group(1)):
                        break                      # first mention is a plain write
                    if re.match(r'^\s*' + re.escape(r) + r'\.compare', t):
                        break                      # cr compare = write
                    if re.match(r'^\s*PPC_STORE_U64\(ctx\.r1\.u32 \+ -\d+, ' + re.escape(r) + r'\.u64\);', t):
                        continue                   # callee-saved spill (benign read); keep looking
                    live.append((r, ln, t.strip()))
                    break
        if live: out.append((name, live))
        i = j
    return out

def main():
    d = sys.argv[1]; detail = '--detail' in sys.argv
    tot = collections.Counter(); nf = 0
    for p in sorted(glob.glob(os.path.join(d, 'ppc_recomp.*.cpp')), key=lambda s: int(re.findall(r'\.(\d+)\.cpp', s)[0])):
        for name, live in scan(p):
            nf += 1
            for r, ln, t in live: tot[r] += 1
            if detail:
                print(f"{os.path.basename(p)} {name}: " + "; ".join(f"{r}@{ln}: {t[:90]}" for r, ln, t in live))
    print(f"functions with live-in locals: {nf}")
    print("by register:", dict(tot.most_common()))

if __name__ == '__main__':
    main()
