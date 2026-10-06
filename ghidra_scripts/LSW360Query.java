// Who references the projection / view matrix globals that come out NaN in our runtime
// (real matrices in Xenia's titles dump): 0x82E8B820 / 0x8301CA28 (projection, 16:9),
// 0x82FEBCD0 / 0x82FEBD50 (two 4x4s), 0x830B6040.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class LSW360Query extends GhidraScript {
    void refs(long addr, String label) throws Exception {
        println(String.format("=== refs to %08X (%s) ===", addr, label));
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(toAddr(addr));
        int n = 0;
        while (it.hasNext() && !monitor.isCancelled()) {
            Reference r = it.next();
            Function f = getFunctionContaining(r.getFromAddress());
            println(String.format("  from %s (%s) type=%s", r.getFromAddress(), f == null ? "-" : f.getName(), r.getReferenceType()));
            if (++n > 25) { println("  ...more"); break; }
        }
        println("  total = " + n);
    }
    @Override
    public void run() throws Exception {
        // Frontend camera object (static) 0x830B60D8: matrix copies at +56, +120, +184.
        refs(0x830B60D8L, "camera obj base");
        refs(0x830B6110L, "camera +56");
        refs(0x830B6150L, "camera +120");
        refs(0x830B6190L, "camera +184");
        refs(0x82EF3900L, "ptr [0x82EF3900] -> camera obj");
        println("=== done ===");
    }
}
