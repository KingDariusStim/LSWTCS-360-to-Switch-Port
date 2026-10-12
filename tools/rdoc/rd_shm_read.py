import os, struct, traceback
import renderdoc as rd
CAP=os.environ["RD_CAP"]; OUT=os.environ["RD_OUT"]; EIDS=[int(x) for x in os.environ["RD_EIDS"].split(",")]
ADDRS=[int(x,16) for x in os.environ["RD_ADDRS"].split(",")]
log=open(OUT,"w")
def p(*a): log.write(" ".join(str(x) for x in a)+"\n"); log.flush()
def main():
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap=rd.OpenCaptureFile(); cap.OpenFile(CAP,"",None)
    st,ctl=cap.OpenCapture(rd.ReplayOptions(),None)
    bufs={b.resourceId:b for b in ctl.GetBuffers()}
    big=[b for b in bufs.values() if b.length>=(256<<20)]
    p("big buffers", [(str(b.resourceId), b.length) for b in big])
    for eid in EIDS:
        ctl.SetFrameEvent(eid, True)
        p("==== EID", eid)
        for b in big:
            for a in ADDRS:
                raw=ctl.GetBufferData(b.resourceId, a, 96)
                be=[struct.unpack(">f", raw[i:i+4])[0] for i in range(0,96,4)]
                le=[struct.unpack("<f", raw[i:i+4])[0] for i in range(0,96,4)]
                p(" buf", b.resourceId, "addr %08X" % a, "raw", raw[:32].hex())
                p("   as BE:", " ".join("%.4g" % x for x in be[:12]))
                p("   as LE:", " ".join("%.4g" % x for x in le[:12]))
    ctl.Shutdown(); cap.Shutdown()
try: main()
except Exception: p(traceback.format_exc())
log.close(); os._exit(0)
