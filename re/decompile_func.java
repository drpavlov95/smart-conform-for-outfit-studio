// Ghidra GhidraScript (Java) — decompile one function (DRAPE_FUNC = hex addr like
// 140260320) and print its pseudo-C plus an ordered list of call targets.
// Append output to drape_decomp.txt (DRAPE_RE_OUT).
//
// @category Drape

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileWriter;

public class decompile_func extends GhidraScript {
    @Override
    public void run() throws Exception {
        String hex = System.getenv("DRAPE_FUNC");
        if (hex == null) { println("set DRAPE_FUNC"); return; }
        Address addr = currentProgram.getAddressFactory().getAddress(hex);
        FunctionManager fm = currentProgram.getFunctionManager();
        Function f = fm.getFunctionContaining(addr);
        if (f == null) { println("no function at " + hex); return; }

        StringBuilder out = new StringBuilder();
        out.append("==== ").append(f.getName()).append(" @ ").append(f.getEntryPoint())
           .append("  (RVA 0x").append(Long.toHexString(
               f.getEntryPoint().getOffset() - currentProgram.getImageBase().getOffset()))
           .append(") ====\n");

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults res = di.decompileFunction(f, 60, monitor);
        if (res != null && res.decompileCompleted()) {
            out.append(res.getDecompiledFunction().getC()).append("\n");
        } else {
            out.append("(decompile failed)\n");
        }

        out.append("---- ordered call targets ----\n");
        Listing listing = currentProgram.getListing();
        InstructionIterator it = listing.getInstructions(f.getBody(), true);
        java.util.LinkedHashSet<String> seen = new java.util.LinkedHashSet<>();
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType() != null && ins.getFlowType().isCall()) {
                for (Reference r : ins.getReferencesFrom()) {
                    Function t = fm.getFunctionAt(r.getToAddress());
                    if (t != null) {
                        long rva = t.getEntryPoint().getOffset() - currentProgram.getImageBase().getOffset();
                        String line = "  " + ins.getAddress() + " -> " + t.getName()
                                    + " @ " + t.getEntryPoint() + "  RVA 0x" + Long.toHexString(rva)
                                    + "  [bytes=" + t.getBody().getNumAddresses() + "]";
                        if (seen.add(line)) out.append(line).append("\n");
                    }
                }
            }
        }
        di.dispose();

        String outDir = System.getenv("DRAPE_RE_OUT");
        if (outDir == null) outDir = ".";
        File rep = new File(outDir, "drape_decomp.txt");
        try (FileWriter fw = new FileWriter(rep, true)) { fw.write(out.toString()); }
        println(out.toString());
    }
}
