// Cutscene timing hunt (2026-10-07): cutscenes advance a fixed 1/30 s per rendered frame (in sync at
// LSWTCS_FPS_CAP=30, video leads at 60). Find who references the fps / cutscene strings and decompile
// those functions to locate the per-frame cutscene step. Output: ghidra_out_cut.txt.
// @category LSWTCS
import java.io.FileWriter;
import java.util.LinkedHashSet;
import java.util.Set;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class LSW360Cut extends GhidraScript {
    StringBuilder sb = new StringBuilder();
    void emit(String s) { sb.append(s).append("\n"); }

    @Override
    public void run() throws Exception {
        long[][] strs = {
            {0x8200DDE8L}, {0x8202BAC8L}, {0x8202CC54L}, {0x8203529CL}, {0x82035538L},
            {0x8202BC58L}, {0x8202BB08L}, {0x8202BB18L}, {0x8200708CL}, {0x820070C4L},
        };
        String[] names = { "fps: %d", "fpsec", "speedmul_maxfps", "Swapped buffers streaming cutscene",
            "PLAYCUTSCENEINST", "Failed to play cutscene track", "cutscene_start", "cutscene_end",
            "CutScenePlaying", "CutSceneFinished" };
        Set<Function> fns = new LinkedHashSet<>();
        for (int i = 0; i < strs.length; i++) {
            emit(String.format("=== refs to %08X (%s) ===", strs[i][0], names[i]));
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(toAddr(strs[i][0]));
            int n = 0;
            while (it.hasNext() && n < 12) {
                Reference r = it.next(); n++;
                Function f = getFunctionContaining(r.getFromAddress());
                emit(String.format("  from %s (%s)", r.getFromAddress(), f == null ? "-" : f.getName()));
                if (f != null) fns.add(f);
            }
            emit("  total=" + n);
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        int k = 0;
        for (Function f : fns) {
            if (++k > 14) break;
            emit("\n// ======== " + f.getName() + " @ " + f.getEntryPoint() + " ========");
            DecompileResults r = dec.decompileFunction(f, 90, monitor);
            emit(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile FAILED");
        }
        try (FileWriter w = new FileWriter(new java.io.File(getSourceFile().getParentFile().getParentFile().getAbsolutePath(), "ghidra_out_cut.txt"))) { w.write(sb.toString()); }
        println("wrote ghidra_out_cut.txt");
    }
}
