"""Rewrite direct guest-memory accesses in XenonRecomp output to go through PPC_HOST() (address fold).

XenonRecomp emits scalar accesses via PPC_LOAD/STORE macros (folded in ppc_context.h) but writes
vector loads/stores (lvx/stvx), lwarx/stwcx reservations and dcbz as raw `base + EXPR`. This turns
every `base + EXPR` (EXPR = up to the first unmatched ')' / ',' / ';') into `PPC_HOST(EXPR)`.
Skips comments and identifiers that merely end in "base". Idempotent (already-folded files have no
`base + ` left). Usage: python fold_recomp_output.py <output dir> [--dry]"""
import glob, os, re, sys

out_dir = sys.argv[1]
dry = "--dry" in sys.argv
pat = re.compile(r"(?<![A-Za-z0-9_])base \+ ")

def rewrite_line(line):
    code, sep, comment = line.partition("//")
    # keep string literals and comments untouched: only rewrite the code part before '//'
    out, i, n = [], 0, 0
    while True:
        m = pat.search(code, i)
        if not m:
            out.append(code[i:]); break
        out.append(code[i:m.start()])
        j, depth = m.end(), 0
        while j < len(code):
            c = code[j]
            if c in "([{": depth += 1
            elif c in ")]}":
                if depth == 0: break
                depth -= 1
            elif c in ",;" and depth == 0: break
            j += 1
        expr = code[m.end():j].rstrip()
        trail = code[m.end() + len(expr):j]
        out.append("PPC_HOST(" + expr + ")" + trail)
        i, n = j, n + 1
    return "".join(out) + sep + comment, n

total_files = total = 0
for path in sorted(glob.glob(os.path.join(out_dir, "*.cpp"))):
    s = open(path, encoding="latin-1", newline="").read()
    if "base + " not in s:
        continue
    nl = "\r\n" if "\r\n" in s else "\n"
    lines = s.split(nl)
    cnt = 0
    for k, ln in enumerate(lines):
        if "base + " in ln:
            lines[k], c = rewrite_line(ln)
            cnt += c
    if cnt:
        total_files += 1; total += cnt
        if not dry:
            open(path, "w", encoding="latin-1", newline="").write(nl.join(lines))
print(f"{'would rewrite' if dry else 'rewrote'} {total} accesses in {total_files} files")
