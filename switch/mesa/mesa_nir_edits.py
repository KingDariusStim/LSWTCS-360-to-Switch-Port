"""LSWTCS: let NV50_PROG_USE_NIR select nouveau's NIR front end on the Switch build of switch-mesa
20.1.0-rc3 (devkitPro's patch hard-wires TGSI on horizon). Default stays TGSI. The disk-cache flags
follow the IR in use, so TGSI and NIR cache entries never mix.
Applied after mesa_cache_edits.py. Usage: python mesa_nir_edits.py <mesa-src-dir>"""
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

edit('src/gallium/drivers/nouveau/nouveau_screen.c', [
    ("#ifndef __SWITCH__\n   if (screen->prefer_nir)\n      driver_flags |= NOUVEAU_SHADER_CACHE_FLAGS_IR_NIR;\n   else\n#endif\n",
     "   if (screen->prefer_nir)\n      driver_flags |= NOUVEAU_SHADER_CACHE_FLAGS_IR_NIR;\n   else\n"),
    ("#ifndef __SWITCH__\n   screen->prefer_nir = debug_get_bool_option(\"NV50_PROG_USE_NIR\", false);\n#else\n   screen->prefer_nir = false;\n#endif\n",
     "   screen->prefer_nir = debug_get_bool_option(\"NV50_PROG_USE_NIR\", false);\n"),
])
print('nir edits applied')
