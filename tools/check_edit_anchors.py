"""Verify that every hand edit landed at the same place in the locals-mode output.

For both worlds (old: ref_folded -> current_edited, new: locals_folded_raw -> locals_patched) take
`diff -U0` per file, and for each block of added lines record an anchor: enclosing PPC_FUNC_IMPL name +
number of PowerPC assembly comment lines ("\t// mnemonic ...", identical in both modes) between the
function start and the block. Removed-line blocks are anchored the same way. Prints every mismatch.

usage: python check_edit_anchors.py <old_ref> <old_edited> <new_ref> <new_edited>
"""
import os, re, subprocess, sys

old_ref, old_ed, new_ref, new_ed = sys.argv[1:5]
ASM = re.compile(r"^\t// [a-z]")
FN = re.compile(r"^PPC_FUNC_IMPL\((\w+)\)")
LOCAL = re.compile(r"\bctx\.(r(?:0|2|11|12|1[4-9]|2\d|3[01])|f(?:0|1[4-9]|2\d|3[01])|"
                   r"v(?:1[4-9]|[2-9]\d|1[01]\d|12[0-7])|cr[0-7]|xer|ctr|reserved)\b")
REST = re.compile(r"\s*__rest(?:gprlr|fpr|vmx)_\d+\(ctx, base\);")

def anchors(lines):
    """per line index -> (function, text of the last asm comment before it, its occurrence # in the function)"""
    out, fn, last, seen = [], "-", "", {}
    for l in lines:
        m = FN.match(l)
        if m: fn, last, seen = m.group(1), "", {}
        out.append((fn, last, seen.get(last, 0)))
        if ASM.match(l):
            last = l.strip(); seen[last] = seen.get(last, 0) + 1
    return out

def edits(ref, ed, norm):
    r = subprocess.run(["diff", "-U0", "--strip-trailing-cr", ref, ed], capture_output=True, text=True, errors="replace").stdout
    a_lines = open(ref, errors="replace").read().split("\n")
    b_lines = open(ed, errors="replace").read().split("\n")
    a_anc, b_anc = anchors(a_lines), anchors(b_lines)
    res = []
    for m in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", r, re.M):
        a0, ac, b0, bc = int(m.group(1)), int(m.group(2) or 1), int(m.group(3)), int(m.group(4) or 1)
        if bc:
            txt = "\n".join(norm(l) for l in b_lines[b0 - 1:b0 - 1 + bc]).strip()
            if txt: res.append(("+", b_anc[b0 - 1], txt))
        if ac:
            txt = "\n".join(norm(l) for l in a_lines[a0 - 1:a0 - 1 + ac]).strip()
            if txt: res.append(("-", a_anc[a0 - 1], txt))
    return res

def norm_old(l): return REST.sub("", LOCAL.sub(r"\1", l))
def norm_new(l): return l

bad = total = 0
for f in sorted(os.listdir(old_ed)):
    if not f.startswith("ppc_recomp.") or not os.path.exists(os.path.join(old_ref, f)): continue
    o = edits(os.path.join(old_ref, f), os.path.join(old_ed, f), norm_old)
    if not o: continue
    n = edits(os.path.join(new_ref, f), os.path.join(new_ed, f), norm_new)
    # Removed lines that only existed in the old world (save/restore calls) are not edits to match.
    o = [e for e in o if not (e[0] == "-" and not norm_old(e[2]).strip())]
    ok = set((k, a, t) for k, a, t in n)
    for e in o:
        total += 1
        if e not in ok:
            bad += 1
            near = [x for x in n if x[2] == e[2]]
            print(f"{f} {e[0]} {e[1]} -> {'found at ' + str(near[0][1]) if near else 'TEXT NOT FOUND'}: {e[2][:110]!r}")
print(f"checked {total} edit blocks, {bad} mismatches")
