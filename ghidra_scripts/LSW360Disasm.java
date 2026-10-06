// Force-disassemble the entire XEX code section of the raw-imported image
// (raw BinaryLoader import has no entry points, so auto-analysis found almost
// nothing). Code range from XenonRecomp config: [0x82130000, 0x82E6527C).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class LSW360Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        long start = 0x82130000L, end = 0x82E6527CL;
        long done = 0;
        for (long a = start; a < end; a += 4) {
            Address addr = toAddr(a);
            if (getInstructionAt(addr) == null && getDataAt(addr) == null) {
                disassemble(addr);   // flows; subsequent already-done addrs skip fast
                done++;
            }
            if ((a & 0xFFFFF) == 0) println(String.format("at %08X", a));
        }
        println("disassemble calls issued: " + done);
    }
}
