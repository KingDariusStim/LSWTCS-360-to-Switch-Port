"""LSWTCS: the nouveau screen's command buffer is a ring of 4 x 512 KB GART buffers (nouveau_screen.c). Before a buffer
is reused the CPU waits until the GPU has executed it (libdrm_nouveau nouveau_bo_map -> nvFenceWait). A heavy level
frame (2000-4000 draws) needs more than those 2 MB, so the render thread could never get far ahead of the GPU: on
hardware (2026-10-09, glthread off) a heavy frame was 27.8 ms render thread (draw 10.3 ms, vs 3 ms in Eden) plus
16.2 ms waiting for the GPU = 44.5 ms, i.e. CPU and GPU mostly took turns. LSWTCS_PUSHBUFS (default 16 = 8 MB;
4 = stock) sets the ring size. Applied after mesa_cache_race_edits.py. Usage: python mesa_pushbuf_edits.py <mesa-src>"""
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

edit('src/gallium/drivers/nouveau/nouveau_screen.c', [
    ("   ret = nouveau_pushbuf_new(screen->client, screen->channel,\n"
     "                             4, 512 * 1024, 1,\n"
     "                             &screen->pushbuf);\n",
     "   /* LSWTCS (switch/mesa/mesa_pushbuf_edits.py): ring of LSWTCS_PUSHBUFS (default 16) command buffers. */\n"
     "   const char *lsw_nbufs = getenv(\"LSWTCS_PUSHBUFS\");\n"
     "   int lsw_nr = lsw_nbufs ? atoi(lsw_nbufs) : 16;\n"
     "   if (lsw_nr < 2 || lsw_nr > 64)\n"
     "      lsw_nr = 16;\n"
     "   printf(\"[MESA] nouveau command buffer ring: %d x 512 KB\\n\", lsw_nr);\n"
     "   ret = nouveau_pushbuf_new(screen->client, screen->channel,\n"
     "                             lsw_nr, 512 * 1024, 1,\n"
     "                             &screen->pushbuf);\n"),
])
print('pushbuf edits applied')
