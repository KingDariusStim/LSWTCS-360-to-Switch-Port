// Ghidra headless script (Java): decompile target functions + callers of the type-node
// initializer to C; dump to ghidra_out.txt. Robust: force-creates functions, always writes file.
// @category LSWTCS
import java.io.FileWriter;
import java.util.LinkedHashSet;
import java.util.Set;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompileTargets extends GhidraScript {
    StringBuilder sb = new StringBuilder();
    DecompInterface dec;

    void emit(String s) { sb.append(s); sb.append("\n"); }

    Function ensureFunction(long va) {
        Address a = toAddr(va);
        Function fn = getFunctionContaining(a);
        if (fn != null) return fn;
        try { if (getInstructionAt(a) == null) disassemble(a); } catch (Exception e) {}
        try { fn = createFunction(a, null); } catch (Exception e) {}
        if (fn == null) fn = getFunctionContaining(a);
        return fn;
    }

    void decompAt(long va, String tag) {
        try {
            Function fn = ensureFunction(va);
            if (fn == null) { emit("\n// 0x" + Long.toHexString(va) + " (" + tag + "): NO FUNCTION (could not create)"); return; }
            emit("\n// ======== 0x" + Long.toHexString(va) + " (" + tag + ")  " + fn.getName()
                 + " @ " + fn.getEntryPoint() + "  params=" + fn.getParameterCount() + " ========");
            DecompileResults r = dec.decompileFunction(fn, 90, monitor);
            if (r != null && r.decompileCompleted())
                emit(r.getDecompiledFunction().getC());
            else
                emit("// decompile FAILED: " + (r == null ? "null" : r.getErrorMessage()));
        } catch (Exception e) { emit("\n// 0x" + Long.toHexString(va) + " EXC: " + e); }
    }

    public void run() throws Exception {
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        emit("// image base = " + currentProgram.getImageBase() + "  min=" + currentProgram.getMinAddress()
             + " max=" + currentProgram.getMaxAddress());
        try {
            // PC shader compile + BIND flow (route-around reference):
            //  0x743e50 = compile fn (D3DXCompileShader vertexProgram/fragmentProgram) [have it]
            //  0x742350 = pre-compile setup (called first)
            //  0x7424c0 = bind (store compiled PS into device, called after)
            decompAt(0x742350L, "precompile-setup");
            decompAt(0x7424c0L, "bind-shader");
            decompAt(0x743e50L, "compile-fn");

            // ⭐ node-init itself: how does the PC initialize a type-graph node's PARENT field?
            // (360 parent field = [node+32]; our construction leaves it 0 → the compile hang.
            //  ROOTFIX comment claims PC writes 0xFFFFFFFF at node-init VA 0x403224.) Need ground truth.
            decompAt(0x403200L, "node-init");
            // The unlink/tree-restructure equivalent (360 sub_827754C8 reads [node+32] as prev-sibling).
            // and the parent-walk that terminates on the sentinel. Decompile a few nearby fns for context.
            decompAt(0x403100L, "near-node-init-1");
            decompAt(0x403300L, "near-node-init-2");

            // Callers of node-init 0x403200 = the type-graph BUILDERS (alloc node, init, set real parent link)
            emit("\n\n// ############### CALLERS of node-init 0x403200 (graph builders) ###############");
            Function initFn = ensureFunction(0x403200L);
            if (initFn != null) {
                Set<Long> seen = new LinkedHashSet<>();
                for (Reference rf : getReferencesTo(initFn.getEntryPoint())) {
                    Function caller = getFunctionContaining(rf.getFromAddress());
                    if (caller != null) seen.add(caller.getEntryPoint().getOffset());
                }
                emit("// direct-call caller count: " + seen.size());
                int n = 0;
                for (Long ce : seen) {
                    if (n++ >= 10) { emit("// ...(" + (seen.size()-10) + " more)"); break; }
                    decompAt(ce, "builder-caller");
                }
            } else emit("// node-init function unavailable; cannot enumerate callers");
        } finally {
            try (FileWriter fw = new FileWriter(new java.io.File(getSourceFile().getParentFile().getParentFile().getAbsolutePath(), "ghidra_out.txt"))) {
                fw.write(sb.toString());
            }
            println("[DecompileTargets] wrote ghidra_out.txt (" + sb.length() + " chars)");
            if (dec != null) dec.dispose();
        }
    }
}
