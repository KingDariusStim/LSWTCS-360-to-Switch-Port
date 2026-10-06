// Ghidra headless (Java): find functions that reference given string literals OR call given
// imported functions, and decompile them to C. Oracle tool for mapping the PC shader compiler.
// @category LSWTCS
import java.io.FileWriter;
import java.util.LinkedHashSet;
import java.util.Set;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class OracleSearch extends GhidraScript {
    StringBuilder sb = new StringBuilder();
    DecompInterface dec;
    Set<Long> done = new LinkedHashSet<>();

    void emit(String s){ sb.append(s); sb.append("\n"); }

    void decomp(Function fn, String why){
        if (fn == null) return;
        long e = fn.getEntryPoint().getOffset();
        if (done.contains(e)) { emit("//   (already dumped " + fn.getName() + ")"); return; }
        done.add(e);
        emit("\n// ======== " + fn.getName() + " @ " + fn.getEntryPoint()
             + "  (" + why + ")  params=" + fn.getParameterCount() + " ========");
        DecompileResults r = dec.decompileFunction(fn, 90, monitor);
        if (r != null && r.decompileCompleted()) emit(r.getDecompiledFunction().getC());
        else emit("// decompile FAILED: " + (r==null?"null":r.getErrorMessage()));
    }

    // find string bytes in memory, decompile functions that reference the address
    void byString(String needle) throws Exception {
        emit("\n\n// ################ refs to bytes: \"" + needle + "\" ################");
        Address found = currentProgram.getMinAddress();
        int hits = 0, decompiled = 0;
        while (found != null && hits < 6) {
            Address[] arr = findBytes(found.next(), needle, 1);
            found = (arr != null && arr.length > 0) ? arr[0] : null;
            if (found == null) break;
            hits++;
            emit("// bytes @ " + found);
            int refs = 0;
            for (Reference rf : getReferencesTo(found)) {
                refs++;
                Function f = getFunctionContaining(rf.getFromAddress());
                if (f != null) { decomp(f, "refs \"" + needle + "\""); decompiled++; }
                else emit("//   ref from " + rf.getFromAddress() + " (no func)");
            }
            if (refs == 0) emit("//   (no refs to this occurrence)");
        }
        if (hits == 0) emit("// (string bytes not found)");
        else if (decompiled == 0) emit("// (found " + hits + " occurrence(s) but no code refs - Ghidra may not have traced them)");
    }

    // decompile CALLERS of the function/thunk at the given address
    void callersOf(long va, String label) throws Exception {
        emit("\n\n// ################ callers of " + label + " @ 0x" + Long.toHexString(va) + " ################");
        Address a = toAddr(va);
        Function self = getFunctionContaining(a);
        long selfE = (self != null) ? self.getEntryPoint().getOffset() : -1;
        int n = 0;
        for (Reference rf : getReferencesTo(a)) {
            Function f = getFunctionContaining(rf.getFromAddress());
            if (f == null) { emit("//   ref from " + rf.getFromAddress() + " (no func)"); continue; }
            if (f.getEntryPoint().getOffset() == selfE) continue; // skip the thunk itself
            decomp(f, "calls " + label);
            if (++n >= 12) { emit("// ...(more callers truncated)"); break; }
        }
        if (n == 0) emit("// (no callers found)");
    }

    public void run() throws Exception {
        dec = new DecompInterface();
        dec.openProgram(currentProgram);
        try {
            byString("%d nodes");
            byString("vertexProgram");
            callersOf(0x74fbe2L, "D3DXCreateEffectCompiler-thunk");
            callersOf(0x74fbdcL, "D3DXCompileShader-thunk");
        } finally {
            try (FileWriter fw = new FileWriter(new java.io.File(getSourceFile().getParentFile().getParentFile().getAbsolutePath(), "ghidra_oracle.txt"))) {
                fw.write(sb.toString());
            }
            println("[OracleSearch] wrote ghidra_oracle.txt (" + sb.length() + " chars, " + done.size() + " funcs)");
            if (dec != null) dec.dispose();
        }
    }
}
