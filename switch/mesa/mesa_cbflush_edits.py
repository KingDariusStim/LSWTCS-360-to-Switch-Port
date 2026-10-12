"""LSWTCS: NVC0_CB_FLUSH_ON_WRITE=1 makes nouveau flush the constant cache (MEM_BARRIER 0x1011 before a draw) only when
a buffer may have been written since the last flush, instead of after every UBO (re)bind.

nvc0_constbufs_validate set cb_dirty ("Force cache flush for UBO") for every buffer-backed constant buffer it binds,
and nvc0_draw_vbo then emits MEM_BARRIER, which waits for the work before it. The Switch port binds per-draw ranges
of one constant arena (2,400 bind changes per frame at the start of Negotiations), so nearly every draw waited:
console 2026-10-11, GLGPUSPLIT=2: freezing the UBO binds saved ~5.8 ms of a 33.4 ms GPU frame. The arena is written
once per frame by the CPU before the draws that read it, so one flush after that write is enough.

With the option: a screen-wide flag (nouveau_screen.cb_written) is set by every path that can write a buffer's
storage: transfer_map with WRITE (glBufferSubData, glMapBufferRange incl. unsynchronized), GPU buffer copies
(nouveau_copy_buffer), user-buffer uploads, buffer invalidation (new storage), clear_buffer. A UBO bind requests the
flush only if the flag is set or the buffer is persistently mapped (written without a map call; as before). Emitting
the barrier clears the flag. Shader writes (SSBO / images / transform feedback) keep using glMemoryBarrier
(nvc0_memory_barrier sets cb_dirty for PIPE_BARRIER_CONSTANT_BUFFER, unchanged). Screen-wide, not per context, so a
write in a shared context also counts. Mesa's default stays off; the port sets it in egl_init (gl_backend.cpp,
LSWTCS_GLCBFLUSH=0 to opt out). Console 2026-10-11: frame p50 34.2 -> 27.9 ms at the Negotiations start, picture and
audio fine.
Applied after the other edits. Usage: python mesa_cbflush_edits.py <mesa-src-dir>"""
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

N = 'src/gallium/drivers/nouveau/'
TAG = '   /* LSWTCS: mesa_cbflush_edits.py */\n'

edit(N + 'nouveau_screen.h', [
    ('struct nouveau_screen {\n   struct pipe_screen base;\n',
     'struct nouveau_screen {\n   struct pipe_screen base;\n'
     '   /* LSWTCS (mesa_cbflush_edits.py): a buffer may have been written since the last constant-cache flush;\n'
     '    * cb_flush_on_write: NVC0_CB_FLUSH_ON_WRITE (0 not read yet, 1 off, 2 on). */\n'
     '   bool cb_written;\n   int cb_flush_on_write;\n'),
])

edit(N + 'nouveau_buffer.c', [
    # glBufferSubData / glMapBufferRange: every write mapping
    ('   if (usage & PIPE_TRANSFER_WRITE)\n      NOUVEAU_DRV_STAT(nv->screen, buf_transfers_wr, 1);\n',
     '   if (usage & PIPE_TRANSFER_WRITE) {\n      NOUVEAU_DRV_STAT(nv->screen, buf_transfers_wr, 1);\n'
     '      nv->screen->cb_written = true;' + TAG + '   }\n'),
    # GPU copy into a buffer
    ('   assert(dst->base.target == PIPE_BUFFER && src->base.target == PIPE_BUFFER);\n',
     '   assert(dst->base.target == PIPE_BUFFER && src->base.target == PIPE_BUFFER);\n'
     '   nv->screen->cb_written = true;' + TAG),
    # user memory uploaded into a GPU buffer
    ('   assert(buf->status & NOUVEAU_BUFFER_STATUS_USER_MEMORY);\n',
     '   assert(buf->status & NOUVEAU_BUFFER_STATUS_USER_MEMORY);\n'
     '   screen->cb_written = true;' + TAG),
    # new storage for a buffer
    ("   int ref = buf->base.reference.count - 1;\n\n   /* Shared buffers shouldn't get reallocated */\n",
     "   int ref = buf->base.reference.count - 1;\n\n"
     "   nv->screen->cb_written = true;" + TAG + "\n"
     "   /* Shared buffers shouldn't get reallocated */\n"),
])

edit(N + 'nvc0/nvc0_surface.c', [
    ('   unsigned width, height, elements;\n\n   assert(res->target == PIPE_BUFFER);\n',
     '   unsigned width, height, elements;\n\n   assert(res->target == PIPE_BUFFER);\n'
     '   nvc0->screen->base.cb_written = true;' + TAG),
])

edit(N + 'nvc0/nvc0_state_validate.c', [
    ('#include "util/u_viewport.h"\n', '#include "util/u_viewport.h"\n#include "util/u_debug.h"\n'),
    ('               nvc0->cb_dirty = 1; /* Force cache flush for UBO. */\n',
     '               /* LSWTCS (mesa_cbflush_edits.py): with NVC0_CB_FLUSH_ON_WRITE=1 only if a buffer may have been\n'
     '                * written since the last flush, or this one is persistently mapped. */\n'
     '               {\n'
     '                  struct nouveau_screen *ns = &nvc0->screen->base;\n'
     '                  if (!ns->cb_flush_on_write)   /* 0: not read yet, 1: off, 2: on */\n'
     '                     ns->cb_flush_on_write = debug_get_bool_option("NVC0_CB_FLUSH_ON_WRITE", false) ? 2 : 1;\n'
     '                  if (ns->cb_flush_on_write == 1 || ns->cb_written ||\n'
     '                      (res->base.flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT))\n'
     '                     nvc0->cb_dirty = 1; /* Force cache flush for UBO. */\n'
     '               }\n'),
])

edit(N + 'nvc0/nvc0_vbo.c', [
    ('      IMMED_NVC0(push, NVC0_3D(MEM_BARRIER), 0x1011);\n      nvc0->cb_dirty = false;\n',
     '      IMMED_NVC0(push, NVC0_3D(MEM_BARRIER), 0x1011);\n      nvc0->cb_dirty = false;\n'
     '      nvc0->screen->base.cb_written = false;' + TAG),
])
print('cbflush edits applied')
