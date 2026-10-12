# RenderDoc capture scripts (PC build)

Headless analysis of `LSWTCSRuntime/build/rdoc/cap_*.rdc` (F12 in the PC build). Each script runs inside RenderDoc's
own Python 3.6 and writes its findings to `RD_OUT`:

    RD_CAP=<capture.rdc> RD_OUT=<out.txt> [other RD_* vars] "C:/Program Files/RenderDoc/qrenderdoc.exe" --python <script.py>

They were written to find the Yoda spike (2026-10-12, `HANDOFF_2026-10-11_cpu.md` §0c). Some still carry that
investigation's constants; edit them for a new case.

| Script | What it does | Inputs |
|---|---|---|
| `rd_pixhistory.py` | Pixel history of one pixel on colour targets (all of them, or the IDs in `RD_TEX`) | `RD_X`, `RD_Y`, `RD_TEX` |
| `rd_bisect_pixel.py` | Binary search for the event that turns a pixel into a given colour (pixel history misses some writers) | texture ID, pixel, event range and colour are hard-coded |
| `rd_explode.py` | Flags draws whose post-VS positions are huge, NaN or straddle w = 0 | none. Post-VS readback of Xenia's shaders is unreliable: compare against a clean capture |
| `rd_draw_constants.py` | Viewport, scissor, every VS constant buffer (float constants as floats, fetch constants as hex) | `RD_EIDS` |
| `rd_disasm_vs.py` | DXBC disassembly of the vertex shader at an event | `RD_EIDS` |
| `rd_shm_read.py` | Reads the shared-memory buffer at guest physical addresses (big- and little-endian floats) | `RD_EIDS`, `RD_ADDRS` (hex) |
| `rd_find_fetch.py` | Finds the draws with a given vertex fetch constant and dumps their streams | fetch addresses hard-coded |
