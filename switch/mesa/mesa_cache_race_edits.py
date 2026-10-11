"""LSWTCS: shader-compile worker crash on hardware (2026-10-09, level load: link_shaders ->
shader_cache_read_program_metadata -> compile_shaders -> _mesa_glsl_compile_shader -> do_dead_code, null deref).
When a program is not in the disk cache, Mesa 20.1's compile_shaders() force-recompiles EVERY shader attached to
it, also ones compiled normally. The port reuses compiled stages across programs (gl_backend.cpp stage reuse) and
links on two worker threads with shared contexts, so a shader's IR was rebuilt on one worker while another linked
a different program from it. (Since the thrd_create fix Mesa's disk cache really stores entries, so deferred
compiles, and this fallback, became common.) Now only shaders whose compile was skipped (COMPILE_SKIPPED) are
compiled, under one lock, rechecking the status inside it: a compiled shader is never rebuilt while shared.
Applied after mesa_glthread_edits.py. Usage: python mesa_cache_race_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    for old, new in pairs:
        if new in s:
            continue   # already applied
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

edit('src/compiler/glsl/shader_cache.cpp', [
    ("static void\n"
     "compile_shaders(struct gl_context *ctx, struct gl_shader_program *prog) {\n"
     "   for (unsigned i = 0; i < prog->NumShaders; i++) {\n"
     "      _mesa_glsl_compile_shader(ctx, prog->Shaders[i], false, false, true);\n"
     "   }\n"
     "}\n",
     "/* LSWTCS (switch/mesa/mesa_cache_race_edits.py): compile only deferred (COMPILE_SKIPPED) shaders, one thread at a\n"
     " * time; shaders are shared between programs linked concurrently on several threads. */\n"
     "static void\n"
     "compile_shaders(struct gl_context *ctx, struct gl_shader_program *prog) {\n"
     "   static std::mutex lsw_fallback_mutex;\n"
     "   std::lock_guard<std::mutex> lock(lsw_fallback_mutex);\n"
     "   for (unsigned i = 0; i < prog->NumShaders; i++) {\n"
     "      if (prog->Shaders[i]->CompileStatus != COMPILE_SKIPPED)\n"
     "         continue;\n"
     "      _mesa_glsl_compile_shader(ctx, prog->Shaders[i], false, false, true);\n"
     "   }\n"
     "}\n"),
    ('#include "util/mesa-sha1.h"\n',
     '#include "util/mesa-sha1.h"\n#include <mutex>\n'),
])
print('cache race edits applied')
