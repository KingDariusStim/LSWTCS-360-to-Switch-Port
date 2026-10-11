// Decompile a list of functions to ghidra_out/decomp_<tag>.c (script args: tag, then hex addresses). Run -readOnly.
// 2026-10-10: the level-script path that switches the Episode 2 disco ball on at hub load in the port
// (sub_824686C0 -> sub_82467318 (script interpreter, recursive) -> sub_82468850 -> sub_823EF250 -> sub_823E93C0/94C0).
// As LSW360Query.fixSaveRestore: auto-analysis marked the __savegprlr/__restgprlr ladders (0x8250E000-0x82512000)
// "no return" and stamped call-terminator overrides on their call sites, which cuts every function off after its
// prologue (size=8); both are cleared over the whole code range and the requested functions rebuilt.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import java.io.PrintWriter;

public class LSW360Decomp extends GhidraScript {
    void fixSaveRestore() throws Exception {
        int fixed = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(0x8250E000L), true)) {
            if (f.getEntryPoint().getOffset() >= 0x82512000L) break;
            if (f.hasNoReturn()) { f.setNoReturn(false); fixed++; }
        }
        int ov = 0;
        for (Instruction ins : currentProgram.getListing().getInstructions(
                 currentProgram.getAddressFactory().getAddressSet(toAddr(0x82130000L), toAddr(0x82E60000L)), true)) {
            if (ins.getFlowOverride() == ghidra.program.model.listing.FlowOverride.CALL_RETURN) {
                ins.setFlowOverride(ghidra.program.model.listing.FlowOverride.NONE); ov++;
            }
        }
        println("no-return cleared on " + fixed + " save/restore entries, " + ov + " call-return overrides cleared");
    }
    Function rebuild(long a) throws Exception {
        Function f = getFunctionAt(toAddr(a));
        if (f != null) removeFunction(f);
        f = createFunction(toAddr(a), null);
        if (f != null) ghidra.app.cmd.function.CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        return f;
    }
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String tag = args.length > 0 ? args[0] : "out";
        fixSaveRestore();
        new java.io.File("ghidra_out").mkdirs();
        PrintWriter out = new PrintWriter("ghidra_out/decomp_" + tag + ".c");
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            long a = Long.parseLong(args[i].replace("0x", ""), 16);
            Function f = rebuild(a);
            if (f == null) { out.println("// no function at " + args[i]); continue; }
            DecompileResults r = d.decompileFunction(f, 180, monitor);
            out.println("// ==== " + f.getName() + " @" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
        }
        out.close();
    }
}
