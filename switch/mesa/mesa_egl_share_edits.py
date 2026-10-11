"""LSWTCS: switch-mesa's EGL driver ignored eglCreateContext's share_context: every context got its own
object namespace (st_api::create_context was passed NULL as the shared context). Programs built on a
shader-compile worker context were then unknown names in the render context (glUseProgram ->
GL_INVALID_VALUE, nothing drawn: black title, Eden 2026-10-08). This passes the share context's
st_context through so contexts created with a share context share textures, buffers, shaders and
programs as EGL specifies. Applied after mesa_fmz_edits.py. Usage: python mesa_egl_share_edits.py <mesa-src-dir>"""
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

edit('src/egl/drivers/switch/egl_switch.c', [
    ("    context->stctx = display->stapi->create_context(display->stapi, display->stmgr, &attribs, &error, NULL);\n",
     "    /* LSWTCS: honour the share context (switch/mesa/mesa_egl_share_edits.py). */\n"
     "    struct st_context_iface *shared = share_list ? switch_egl_context(share_list)->stctx : NULL;\n"
     "    context->stctx = display->stapi->create_context(display->stapi, display->stmgr, &attribs, &error, shared);\n"),
])
print('egl share edits applied')
