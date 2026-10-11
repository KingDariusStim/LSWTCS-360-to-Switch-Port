"""Move function-entry hand edits that `patch` put into the wrong function (locals-mode functions start
with register declarations, so the hunk context no longer matched) to the right place: directly before the
first assembly comment line of the function they belong to.

Input: anchors.txt lines from check_edit_anchors.py of the form
  <file> + ('<F>', '', 0) -> found at ('<G>', ...): '<first 110 chars of the block>'
The full block text is taken from the old edited file (the block that starts at F's entry).
usage: python fix_entry_edits.py <anchors.txt> <old_ref> <old_edited> <new_edited>
"""
import ast, os, re, subprocess, sys

anc_file, old_ref, old_ed, new_ed = sys.argv[1:5]
ASM = re.compile(r"^\t// [a-z]")
LOCAL = re.compile(r"\bctx\.(r(?:0|2|11|12|1[4-9]|2\d|3[01])|f(?:0|1[4-9]|2\d|3[01])|"
                   r"v(?:1[4-9]|[2-9]\d|1[01]\d|12[0-7])|cr[0-7]|xer|ctr|reserved)\b")
REST = re.compile(r"\s*__rest(?:gprlr|fpr|vmx)_\d+\(ctx, base\);")
LINE = re.compile(r"^(\S+) \+ \('(\w+)', '', 0\) -> found at \('(\w+)'")

def func_range(lines, name):
    s = next(i for i, l in enumerate(lines) if l.startswith(f"PPC_FUNC_IMPL({name})"))
    e = next((i for i in range(s + 1, len(lines)) if lines[i].startswith("PPC_FUNC_IMPL(")), len(lines))
    return s, e

def entry_block(f, fn):
    """added lines at the entry of fn in the old edited file (before its first asm comment)"""
    r = subprocess.run(["diff", "-U0", "--strip-trailing-cr", os.path.join(old_ref, f), os.path.join(old_ed, f)],
                       capture_output=True, text=True, errors="replace").stdout
    ed = open(os.path.join(old_ed, f), errors="replace").read().split("\n")
    s, e = func_range(ed, fn)
    first_asm = next(i for i in range(s, e) if ASM.match(ed[i]))
    blocks = []
    for m in re.finditer(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", r, re.M):
        b0, bc = int(m.group(1)), int(m.group(2) or 1)
        if bc and s < b0 - 1 < first_asm:
            blocks.append([REST.sub("", LOCAL.sub(r"\1", l)) for l in ed[b0 - 1:b0 - 1 + bc]])
    return blocks

moved = 0
for line in open(anc_file):
    m = LINE.match(line)
    if not m: continue
    f, fn, wrong = m.groups()
    path = os.path.join(new_ed, f)
    new = open(path, errors="surrogateescape").read().split("\n")
    for blk in entry_block(f, fn):
        if not "".join(blk).strip(): continue
        # remove it from the wrong function
        ws, we = func_range(new, wrong)
        at = next((i for i in range(ws, we) if new[i:i + len(blk)] == blk), None)
        if at is None:
            print(f"  {f} {fn}: block not found in {wrong}, skipped"); continue
        del new[at:at + len(blk)]
        s, e = func_range(new, fn)
        first_asm = next(i for i in range(s, e) if ASM.match(new[i]))
        new[first_asm:first_asm] = blk
        moved += 1
    open(path, "w", errors="surrogateescape", newline="\n").write("\n".join(new))
print(f"moved {moved} blocks")
