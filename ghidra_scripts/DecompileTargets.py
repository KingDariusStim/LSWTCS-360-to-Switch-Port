# Ghidra headless post-script: decompile target functions to C and dump to a file.
# Jython (GhidraScript). Targets are passed via the script args.
# @category LSWTCS
import os
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

OUT = os.path.join(os.path.dirname(os.path.dirname(getSourceFile().getAbsolutePath())), "ghidra_out.txt")

# Addresses of interest in the PC EXE (loaded VA, image base 0x400000):
#  0x403200 = type-node initializer (writes -1 sentinel to [+0x20]/[+0x24])
#  0x747810 = NU2 HLSL source generator (references "struct VertexInput { ")
TARGETS = [0x403200, 0x403240, 0x747810]

prog = getCurrentProgram()
fm = prog.getFunctionManager()
af = prog.getAddressFactory()
space = af.getDefaultAddressSpace()

dec = DecompInterface()
dec.openProgram(prog)
mon = ConsoleTaskMonitor()

lines = []
def emit(s):
    lines.append(s)

# Also: find functions that WRITE to [reg+0x20] near the generator, to locate the
# real type-graph builder. We rely on auto-analysis having created functions.
emit("=== Decompiled targets ===")
for va in TARGETS:
    addr = space.getAddress(va)
    fn = fm.getFunctionContaining(addr)
    if fn is None:
        emit("\n// 0x%X: NO FUNCTION (analysis missed it)" % va)
        continue
    emit("\n// ---- 0x%X  %s  @ %s ----" % (va, fn.getName(), fn.getEntryPoint()))
    res = dec.decompileFunction(fn, 60, mon)
    if res and res.decompileCompleted():
        emit(res.getDecompiledFunction().getC())
    else:
        emit("// decompile FAILED: %s" % (res.getErrorMessage() if res else "no result"))

# Dump callees of the type-node initializer's callers and the generator, to map the subsystem
emit("\n=== Functions calling the type-node initializer 0x403200 ===")
init_addr = space.getAddress(0x403200)
init_fn = fm.getFunctionContaining(init_addr)
if init_fn:
    refs = getReferencesTo(init_fn.getEntryPoint())
    seen = set()
    for r in refs:
        caller = fm.getFunctionContaining(r.getFromAddress())
        if caller and caller.getEntryPoint() not in seen:
            seen.add(caller.getEntryPoint())
            emit("  caller: %s @ %s" % (caller.getName(), caller.getEntryPoint()))

with open(OUT, "w") as f:
    f.write("\n".join(lines))
print("[DecompileTargets] wrote %s (%d lines)" % (OUT, len(lines)))
