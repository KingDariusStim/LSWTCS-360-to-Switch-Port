import os, traceback
import renderdoc as rd
CAP=os.environ["RD_CAP"]; OUT=os.environ["RD_OUT"]; EIDS=[int(x) for x in os.environ["RD_EIDS"].split(",")]
log=open(OUT,"w")
def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
    for eid in EIDS:
        ctl.SetFrameEvent(eid, True)
        pipe=ctl.GetPipelineState()
        refl=pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        tgt=ctl.GetDisassemblyTargets(True)[0]
        log.write("==== EID %d VS %s target %s\n" % (eid, refl.resourceId, tgt))
        log.write(ctl.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, tgt))
    ctl.Shutdown(); cap.Shutdown()
try: main()
except Exception: log.write(traceback.format_exc())
log.close(); os._exit(0)
