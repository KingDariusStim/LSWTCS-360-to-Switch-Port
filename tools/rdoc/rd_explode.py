import os, struct, sys, traceback
import renderdoc as rd

CAP = os.environ.get("RD_CAP")
OUT = os.environ.get("RD_OUT")
log = open(OUT, "w")
def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n"); log.flush()

def walk(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall: out.append(a)
        walk(a.children, out)

def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAP, "", None) != rd.ResultCode.Succeeded: p("open failed"); return
    st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if st != rd.ResultCode.Succeeded: p("replay failed", st); return
    draws = []
    walk(ctl.GetRootActions(), draws)
    p("draws", len(draws))
    flagged = 0
    for a in draws:
        ctl.SetFrameEvent(a.eventId, True)
        m = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if m.numIndices == 0 or m.vertexResourceId == rd.ResourceId.Null(): continue
        n = m.numIndices
        idx = list(range(n))
        if m.indexResourceId != rd.ResourceId.Null() and m.indexByteStride:
            raw = ctl.GetBufferData(m.indexResourceId, m.indexByteOffset, n * m.indexByteStride)
            fmt = "<%d%s" % (n, "H" if m.indexByteStride == 2 else "I")
            if len(raw) >= n * m.indexByteStride: idx = list(struct.unpack(fmt, raw[:n * m.indexByteStride]))
        nv = max(idx) + 1 if idx else 0
        raw = ctl.GetBufferData(m.vertexResourceId, m.vertexByteOffset, nv * m.vertexByteStride)
        bad = []
        ws = []
        for v in range(nv):
            o = v * m.vertexByteStride
            if o + 16 > len(raw): break
            x, y, z, w = struct.unpack_from("<4f", raw, o)
            ws.append(w)
            if w != w or x != x or abs(w) < 1e-6 or abs(x) > 1e4 or abs(y) > 1e4 or abs(w) > 1e4 or (abs(x) > 20 * abs(w)) or (abs(y) > 20 * abs(w)):
                bad.append((v, round(x, 3), round(y, 3), round(z, 3), round(w, 5)))
        neg = sum(1 for w in ws if w <= 0)
        if bad or (0 < neg < len(ws)):
            flagged += 1
            if flagged <= 60:
                pipe = ctl.GetPipelineState()
                vs = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
                p("EID", a.eventId, "verts", nv, "idx", n, "bad", len(bad), "w<=0", neg, "name", a.GetName(ctl.GetStructuredFile()),
                  "vs", vs.resourceId if vs else None)
                for b in bad[:6]: p("    ", b)
    p("flagged", flagged)
    ctl.Shutdown(); cap.Shutdown()

try:
    main()
except Exception:
    p(traceback.format_exc())
log.close()
os._exit(0)
