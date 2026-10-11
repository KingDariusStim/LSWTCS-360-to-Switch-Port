// Identify the audio thread's hot function sub_827E2208 (39% of audio-thread samples in the hub,
// 2026-10-08): caller chain (3 levels), strings the chain references, and decompiled C of it and
// its helpers, written to ghidra_out/audio_827E2208.c.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class LSW360Query extends GhidraScript {
    Set<Function> callers(Function f) {
        Set<Function> out = new LinkedHashSet<>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (it.hasNext()) {
            Reference r = it.next();
            Function c = getFunctionContaining(r.getFromAddress());
            if (c != null) out.add(c);
        }
        return out;
    }
    void strings(Function f, String indent) {
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            for (Reference r : ins.getReferencesFrom()) {
                Data d = getDataAt(r.getToAddress());
                String s = null;
                if (d != null && d.hasStringValue()) s = d.getDefaultValueRepresentation();
                else {
                    try {
                        byte[] b = getBytes(r.getToAddress(), 48);
                        int n = 0; while (n < b.length && b[n] >= 0x20 && b[n] < 0x7F) n++;
                        if (n >= 5 && (n == b.length || b[n] == 0)) s = "\"" + new String(b, 0, n) + "\"";
                    } catch (Exception e) { }
                }
                if (s != null) println(indent + "  str @" + r.getToAddress() + " " + s);
            }
        }
    }
    void chain(Function f, int depth, String indent, Set<Function> seen) {
        if (depth > 3 || !seen.add(f)) return;
        println(indent + f.getName() + " @" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
        strings(f, indent);
        for (Function c : callers(f)) chain(c, depth + 1, indent + "  ", seen);
    }
    // Run with -readOnly: the fixes below are not saved to the project.
    // The __savegprlr/__restgprlr/__savefpr/__restfpr/__savevmx ladders at 0x8250F1xx-0x8251xxxx are
    // marked "no return" by auto-analysis, which truncates every function after its prologue.
    void fixSaveRestore() throws Exception {
        int fixed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(0x8250E000L), true)) {
            if (f.getEntryPoint().getOffset() >= 0x82512000L) break;
            if (f.hasNoReturn()) { f.setNoReturn(false); fixed++; }
        }
        println("no-return cleared on " + fixed + " save/restore entries");
        // Analysis also stamped CALL_TERMINATOR flow overrides on the call sites: clear them in the
        // audio code range.
        int ov = 0;
        for (Instruction ins : currentProgram.getListing().getInstructions(
                 currentProgram.getAddressFactory().getAddressSet(toAddr(0x827D0000L), toAddr(0x82800000L)), true)) {
            if (ins.getFlowOverride() != ghidra.program.model.listing.FlowOverride.NONE) {
                ins.setFlowOverride(ghidra.program.model.listing.FlowOverride.NONE); ov++;
            }
        }
        println("flow overrides cleared: " + ov);
    }
    Function rebuild(long a) throws Exception {
        Function f = getFunctionAt(toAddr(a));
        if (f != null) removeFunction(f);
        f = createFunction(toAddr(a), null);
        if (f != null) {
            ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        }
        return f;
    }
    @Override
    public void run() throws Exception {
        fixSaveRestore();
        long[] fns = {0x827E3940L, 0x827E2208L, 0x827E0810L, 0x827E0EA8L, 0x827E0FD8L, 0x827E10A8L, 0x827E1330L,
                      0x827E1400L, 0x827E1728L, 0x827E1890L, 0x827E15C0L, 0x827E51D0L};
        for (long a : fns) rebuild(a);
        Function hot = getFunctionAt(toAddr(0x827E3940L));
        println("=== caller chain (from the direct caller 827E3940) ===");
        chain(hot, 0, "", new LinkedHashSet<>());
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        new java.io.File("ghidra_out").mkdirs();
        try (PrintWriter pw = new PrintWriter("ghidra_out/audio_827E2208.c")) {
            for (long a : fns) {
                Function f = getFunctionAt(toAddr(a));
                if (f == null) f = createFunction(toAddr(a), null);
                if (f == null) { pw.println("// no function at " + Long.toHexString(a)); continue; }
                DecompileResults r = di.decompileFunction(f, 300, monitor);
                pw.println("// ===== " + f.getName() + " @" + f.getEntryPoint());
                pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
        println("=== done ===");
    }
}
