"""LSWTCS: NV50_MUL_ZERO_WINS=1 makes nouveau compile every float multiplication (MUL, MAD/FMA, DPn,
LRP) with Maxwell's FMZ mode: 0 * anything = 0, also for Inf / NaN, which is the Xenos (Shader Model
3) rule. The Switch port's translated shaders then use plain multiplications instead of emulating the
rule with abs/min/compare/select around every product (Xenia's ZeroIfAnyOperandIsZero; about a fifth
of the GLSL Mesa has to compile). This is the same TGSI_PROPERTY_MUL_ZERO_WINS path Gallium Nine
uses, enabled for all shaders since every shader the port runs is a translated Xenos shader.
nouveau's backend output is not in the disk cache (TGSI is), so cache entries are unaffected.
Applied after mesa_cache_edits.py. Usage: python mesa_fmz_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    for old, new in pairs:
        if s.count(new) == 1 and s.count(old) == 0:
            continue   # already applied
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

edit('src/gallium/drivers/nouveau/codegen/nv50_ir_from_tgsi.cpp', [
    ('#include "tgsi/tgsi_util.h"\n',
     '#include "tgsi/tgsi_util.h"\n#include "util/u_debug.h"\n'),
    ("   info->io.viewportId = -1;\n\n   info->immd.data = (uint32_t *)MALLOC(scan.immediate_count * 16);\n",
     "   info->io.viewportId = -1;\n\n"
     "   /* LSWTCS: NV50_MUL_ZERO_WINS=1 -> 0 * x = 0 for every float multiplication (FMZ), as if each\n"
     "    * shader declared TGSI_PROPERTY_MUL_ZERO_WINS (switch/mesa/mesa_fmz_edits.py). */\n"
     "   {\n      static int zero_wins = -1;\n      if (zero_wins < 0)\n"
     "         zero_wins = debug_get_bool_option(\"NV50_MUL_ZERO_WINS\", false);\n"
     "      if (zero_wins)\n         info->io.mul_zero_wins = true;\n   }\n\n"
     "   info->immd.data = (uint32_t *)MALLOC(scan.immediate_count * 16);\n"),
])
print('fmz edits applied')
