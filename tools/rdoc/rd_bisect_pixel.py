import os, traceback
import renderdoc as rd
CAP=os.environ["RD_CAP"]; OUT=os.environ["RD_OUT"]
log=open(OUT,"w")
def p(*a): log.write(" ".join(str(x) for x in a)+"\n"); log.flush()
def walk(acts,out):
    for a in acts:
        out.append(a); walk(a.children,out)
def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
    tex=[t for t in ctl.GetTextures() if str(t.resourceId)=="ResourceId::306"][0].resourceId
    acts=[]; walk(ctl.GetRootActions(),acts)
    eids=sorted(set(a.eventId for a in acts if 1440 <= a.eventId <= 2509))
    names={a.eventId:a for a in acts}
    def val(e):
        ctl.SetFrameEvent(e, True)
        v=ctl.PickPixel(tex,1100,150,rd.Subresource(0,0,0),rd.CompType.Typeless).floatValue
        return v
    lo,hi=0,len(eids)-1
    p("start", eids[lo], val(eids[lo])[:3], "end", eids[hi], val(eids[hi])[:3])
    while hi-lo>1:
        mid=(lo+hi)//2
        v=val(eids[mid])
        if abs(v[0]-0.376)<0.01 and abs(v[1]-0.443)<0.01: hi=mid
        else: lo=mid
    sf=ctl.GetStructuredFile()
    for e in eids[max(0,lo-2):hi+2]:
        a=names[e]
        p("EID", e, a.GetName(sf), "flags", int(a.flags), "val", ["%.3f"%x for x in val(e)[:3]])
    ctl.SetFrameEvent(eids[hi], True)
    pipe=ctl.GetPipelineState()
    vs=pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    p("culprit EID", eids[hi], "VS", vs.resourceId if vs else None)
    m=ctl.GetPostVSData(0,0,rd.MeshDataStage.VSOut)
    p("numIndices", m.numIndices)
    ctl.Shutdown(); cap.Shutdown()
try: main()
except Exception: p(traceback.format_exc())
log.close(); os._exit(0)
