// NPC/creature spawn code (2026-10-11).
// Camera code for the "show the goal" camera that swings to a random stud (2026-10-10): every function that
// references the globals holding the view matrix (found by LSWTCS_CAMFIND in Eden: 0x82FC6520, 0x82FE78B0,
// 0x82FF5540, 0x830B6110 / 6190 / 61D0; +-0x40 around each) or the goal-camera script parameter strings
// (cam_lookatplayers / cam_blend_time / cam_wait / cam_spline, CameraCut, DynamicCameraCut, EndCameraCut),
// with the instruction addresses and one level of callers. Output: ghidra_out/npc_refs.txt.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

public class LSW360Npc extends GhidraScript {
    PrintWriter out;
    void p(String s) { out.println(s); println(s); }
    Set<Function> callers(Function f) {
        Set<Function> res = new LinkedHashSet<>();
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
        while (it.hasNext()) {
            Function c = getFunctionContaining(it.next().getFromAddress());
            if (c != null) res.add(c);
        }
        return res;
    }
    void refsTo(String label, long lo, long hi) {
        Map<Function, StringBuilder> by = new LinkedHashMap<>();
        for (long a = lo; a < hi; a += 4) {
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(toAddr(a));
            while (it.hasNext()) {
                Reference r = it.next();
                Function f = getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                by.computeIfAbsent(f, k -> new StringBuilder()).append(String.format(" %s->%08X", r.getFromAddress(), a));
            }
        }
        p("== " + label + String.format(" [%08X, %08X): %d functions", lo, hi, by.size()));
        for (Map.Entry<Function, StringBuilder> e : by.entrySet()) {
            Function f = e.getKey();
            StringBuilder cs = new StringBuilder();
            for (Function c : callers(f)) cs.append(" ").append(c.getEntryPoint());
            p("  " + f.getName() + " @" + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " refs:" + e.getValue()
              + " | callers:" + cs);
        }
    }
    @Override
    public void run() throws Exception {
        new java.io.File("ghidra_out").mkdirs();
        out = new PrintWriter("ghidra_out/npc_refs.txt");
        String[] names = {"%sLevels\\%s\\%s\\%s.ai2", "%s.ai2", "CreateCreatures", "CreateSplineCreatures", "GameObjectHelper::CreateObject - Failed!! creature id=%d, player=%d, flags=0x%x", "AIEDITOR_CREATURES", "BeenSpawned", "SpawnCount", "CREATURE", "No characters configured from '%s."};
        for (String n : names) {
            Address a = find(null, (n + "\0").getBytes());
            int k = 0;
            while (a != null && k < 4) {
                refsTo("string \"" + n + "\"", a.getOffset(), a.getOffset() + 4);
                a = find(a.add(1), (n + "\0").getBytes());
                ++k;
            }
        }
        out.close();
    }
}
