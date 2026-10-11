"""Port the hand edits of the recompiled game code (a unified diff against the folded reference output)
to output generated with XenonRecomp's *_as_local options.

In locals mode the recompiler names these registers as function locals instead of ctx fields:
r0 r2 r11 r12 r14-r31, f0 f14-f31, v14-v127, cr0-cr7, xer, ctr, reserved. Every hunk line (context,
removed and added) gets the same renaming so the hunk matches the new output and the added code refers
to the locals the surrounding code really uses.

usage: python port_edits.py <in.patch> <out.patch>
Prints added lines that pass ctx to a non-guest helper: such helpers read/write ctx registers that may
now live in locals and must be reviewed by hand.
"""
import re, sys

LOCAL = re.compile(r"\bctx\.(r(?:0|2|11|12|1[4-9]|2\d|3[01])|f(?:0|1[4-9]|2\d|3[01])|"
                   r"v(?:1[4-9]|[2-9]\d|1[01]\d|12[0-7])|cr[0-7]|xer|ctr|reserved)\b")
REST = re.compile(r"\s*__rest(?:gprlr|fpr|vmx)_\d+\(ctx, base\);")
HELPER = re.compile(r"\b(?!sub_|__imp__sub_|PPC_)([A-Za-z_]\w*)\s*\(\s*ctx\s*,")

src, dst = sys.argv[1], sys.argv[2]
out, review, cur = [], [], None
for line in open(src, encoding="utf-8", errors="surrogateescape"):
    if line.startswith("+++ "):
        cur = line.split()[1]
    elif line[:1] in " +-" and not line.startswith("--- ") and cur and "ppc_recomp." in cur:
        new = LOCAL.sub(r"\1", line)
        # Early-return guards restored the non-volatile registers the prologue saved; in locals mode the
        # prologue saves nothing (the registers are the caller's own host locals), so drop the restore.
        if new.startswith("+"):
            new = REST.sub("", new)
        if line.startswith("+") and HELPER.search(new):
            review.append(f"{cur}: {new.strip()[:160]}")
        line = new
    out.append(line)
open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="\n").write("".join(out))
print(f"wrote {dst}; {len(review)} added lines pass ctx to a non-guest helper:")
for r in review:
    print("  " + r)
