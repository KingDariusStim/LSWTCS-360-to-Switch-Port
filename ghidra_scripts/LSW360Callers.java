// Caller tree (2026-10-11): for each hex address given as a script argument, the function containing it and its
// callers up to 5 levels (each function printed once). Output: ghidra_out/callers_<first address>.txt. Run -readOnly.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class LSW360Callers extends GhidraScript {
    PrintWriter out;
    Set<Function> seen = new HashSet<>();
    void walk(Function f, int depth) {
        StringBuilder ind = new StringBuilder();
        for (int i = 0; i < depth; ++i) ind.append("  ");
        boolean again = !seen.add(f);
        out.println(ind + f.getEntryPoint().toString() + " size=" + f.getBody().getNumAddresses() + (again ? " (seen)" : ""));
        if (again || depth >= 5) return;
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (it.hasNext()) {
            var r = it.next();
            Function c = getFunctionContaining(r.getFromAddress());
            if (c == null) { out.println(ind + "  <no function> " + r.getFromAddress()); continue; }
            out.print(ind + "  from " + r.getFromAddress() + ": ");
            walk(c, depth + 1);
        }
    }
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        new java.io.File("ghidra_out").mkdirs();
        out = new PrintWriter("ghidra_out/callers_" + args[0] + ".txt");
        for (String a : args) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(a, 16)));
            out.println("== " + a + ": " + (f == null ? "no function" : f.getName()));
            if (f != null) walk(f, 0);
        }
        out.close();
    }
}
