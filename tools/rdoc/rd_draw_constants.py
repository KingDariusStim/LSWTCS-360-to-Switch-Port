import os, struct, traceback
import renderdoc as rd

CAP = os.environ["RD_CAP"]; OUT = os.environ["RD_OUT"]; EIDS = [int(x) for x in os.environ["RD_EIDS"].split(",")]
log = open(OUT, "w")
def p(*a): log.write(" ".join(str(x) for x in a) + "\n"); log.flush()

def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    cap.OpenFile(CAP, "", None)
    st, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    for eid in EIDS:
        ctl.SetFrameEvent(eid, True)
        pipe = ctl.GetPipelineState()
        p("==== EID", eid)
        vp = pipe.GetViewport(0); sc = pipe.GetScissor(0)
        p("viewport", vp.x, vp.y, vp.width, vp.height, "scissor", sc.x, sc.y, sc.width, sc.height)
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        p("VS", refl.resourceId, refl.debugInfo.files[0].filename if refl and len(refl.debugInfo.files) else "")
        for i, cb in enumerate(refl.constantBlocks):
            b = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
            d = b.descriptor
            p("cb", i, cb.name, "bytes", cb.byteSize, "bind", d.resource, "off", d.byteOffset, "size", d.byteSize)
            if d.resource == rd.ResourceId.Null(): continue
            raw = ctl.GetBufferData(d.resource, d.byteOffset, min(d.byteSize if d.byteSize else 8192, 8192))
            nreg = len(raw) // 16
            big = []
            for r in range(nreg):
                v = struct.unpack_from("<4f", raw, r * 16)
                if any(x != x or abs(x) > 1e6 for x in v): big.append((r, v))
            p("   regs", nreg, "huge/NaN regs", len(big))
            for r, v in big[:12]: p("     c%d" % r, v)
            if "fetch" in cb.name:
                for r in range(48):
                    w = struct.unpack_from("<4I", raw, r * 16)
                    p("     f%02d %08X %08X %08X %08X" % ((r,) + w))
            if "loat" in cb.name:
                for r in range(min(nreg, 512)):
                    v = struct.unpack_from("<4f", raw, r * 16)
                    p("     c%03d % .5g % .5g % .5g % .5g" % ((r,) + v))
        # vertex inputs of the first few vertices
        ins = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSIn)
        for vb in pipe.GetVBuffers()[:4]:
            p("vb", vb.resourceId, "off", vb.byteOffset, "stride", vb.byteStride, "size", vb.byteSize)
    ctl.Shutdown(); cap.Shutdown()
try: main()
except Exception: p(traceback.format_exc())
log.close(); os._exit(0)
