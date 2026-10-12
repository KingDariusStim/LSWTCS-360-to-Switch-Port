import os, traceback
import renderdoc as rd
CAP=os.environ["RD_CAP"]; OUT=os.environ["RD_OUT"]; X=int(os.environ["RD_X"]); Y=int(os.environ["RD_Y"])
log=open(OUT,"w")
def p(*a): log.write(" ".join(str(x) for x in a)+"\n"); log.flush()
def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
    ctl.SetFrameEvent(10**9, True)
    for t in ctl.GetTextures():
        if str(t.resourceId) not in os.environ.get('RD_TEX','').split(','): continue
        if t.creationFlags & rd.TextureCategory.DepthTarget: continue
        if not (t.creationFlags & (rd.TextureCategory.ColorTarget | rd.TextureCategory.SwapBuffer)): continue
        for (x, y) in ((X, Y), (X * t.width // 1280, Y)):
            if x >= t.width or y >= t.height: continue
            hist = ctl.PixelHistory(t.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            if not hist: continue
            p("tex", t.resourceId, t.width, t.height, "msaa", t.msSamp, "fmt", t.format.Name(), "pixel", x, y, "mods", len(hist))
            for h in hist:
                c = h.shaderOut.col.floatValue
                p("   EID", h.eventId, "passed", h.Passed(), "prim", h.primitiveID, "out %.3f %.3f %.3f %.3f" % tuple(c[:4]),
                  "post %.3f %.3f %.3f" % tuple(h.postMod.col.floatValue[:3]))
            break
    ctl.Shutdown(); cap.Shutdown()
try: main()
except Exception: p(traceback.format_exc())
log.close(); os._exit(0)
