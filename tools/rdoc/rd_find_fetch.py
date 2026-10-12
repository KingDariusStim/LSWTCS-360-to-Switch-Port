import os, struct, traceback
import renderdoc as rd
CAP=os.environ["RD_CAP"]; OUT=os.environ["RD_OUT"]
log=open(OUT,"w")
def p(*a): log.write(" ".join(str(x) for x in a)+"\n"); log.flush()
def walk(acts,out):
    for a in acts:
        if a.flags & rd.ActionFlags.Drawcall: out.append(a)
        walk(a.children,out)
def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
    shm=[b for b in ctl.GetBuffers() if b.length>=(256<<20)][0].resourceId
    draws=[]; walk(ctl.GetRootActions(),draws)
    for a in draws:
        ctl.SetFrameEvent(a.eventId, True)
        pipe=ctl.GetPipelineState()
        refl=pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if not refl: continue
        for i,cb in enumerate(refl.constantBlocks):
            if "fetch" not in cb.name: continue
            d=pipe.GetConstantBlock(rd.ShaderStage.Vertex,i,0).descriptor
            raw=ctl.GetBufferData(d.resource,d.byteOffset,768)
            f=struct.unpack_from("<4I",raw,47*16)
            if (f[2] & ~3) in (0x1013EB20,) or (f[0] & ~3) == 0x101564FC:
                s1=ctl.GetBufferData(shm,(f[0]&~3),48); s2=ctl.GetBufferData(shm,(f[2]&~3),36)
                p("EID",a.eventId,"VS",refl.resourceId,"f47 %08X %08X %08X %08X" % f)
                p("   stream@%08X BE:" % (f[0]&~3), " ".join("%.4g" % struct.unpack(">f",s1[k:k+4])[0] for k in range(0,48,4)))
                p("   stream@%08X BE:" % (f[2]&~3), " ".join("%.4g" % struct.unpack(">f",s2[k:k+4])[0] for k in range(0,12,4)), s2[12:36].hex())
    ctl.Shutdown(); cap.Shutdown()
try: main()
except Exception: p(traceback.format_exc())
log.close(); os._exit(0)
