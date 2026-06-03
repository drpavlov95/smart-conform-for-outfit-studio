// Ghidra GhidraScript (Java) — find the function containing a string (DRAPE_STR)
// and decompile it plus its SMALL non-library callees recursively (depth DRAPE_DEPTH,
// default 3; only functions with body < DRAPE_MAXBYTES, default 700). Helps trace
// small wrapper chains like OnClearSlider -> ClearUnmaskedDiff -> ZeroVertDiff.
// Writes drape_tree.txt (DRAPE_RE_OUT).
//
// @category Drape

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import java.io.File;
import java.io.FileWriter;
import java.util.*;

public class decompile_tree extends GhidraScript {
    DecompInterface di;
    FunctionManager fm;
    Listing listing;
    StringBuilder out = new StringBuilder();
    long imageBase;
    int maxBytes = 700;

    @Override
    public void run() throws Exception {
        fm = currentProgram.getFunctionManager();
        listing = currentProgram.getListing();
        imageBase = currentProgram.getImageBase().getOffset();
        ReferenceManager rm = currentProgram.getReferenceManager();
        String needle = System.getenv("DRAPE_STR");
        String funcHex = System.getenv("DRAPE_FUNC");
        int depth = 3;
        try { depth = Integer.parseInt(System.getenv("DRAPE_DEPTH")); } catch (Exception e) {}
        try { maxBytes = Integer.parseInt(System.getenv("DRAPE_MAXBYTES")); } catch (Exception e) {}

        di = new DecompInterface();
        di.openProgram(currentProgram);

        Set<Function> roots = new LinkedHashSet<>();
        if (funcHex != null) {
            Address a = currentProgram.getAddressFactory().getAddress(funcHex);
            Function f = fm.getFunctionContaining(a);
            if (f != null) roots.add(f);
        } else if (needle != null) {
            DataIterator dit = listing.getDefinedData(true);
            while (dit.hasNext()) {
                Data d = dit.next();
                if (d == null) continue;
                DataType dt = d.getDataType();
                if (dt == null) continue;
                String tn = dt.getName().toLowerCase();
                if (!(tn.contains("string") || tn.contains("unicode") || tn.contains("char"))) continue;
                Object v = d.getValue();
                if (v == null) continue;
                if (v.toString().contains(needle)) {
                    for (Reference r : rm.getReferencesTo(d.getAddress())) {
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        if (f != null) roots.add(f);
                    }
                }
            }
        }

        Set<Address> seen = new HashSet<>();
        for (Function f : roots) decompile(f, depth, seen);

        di.dispose();
        String outDir = System.getenv("DRAPE_RE_OUT");
        if (outDir == null) outDir = ".";
        File rep = new File(outDir, "drape_tree.txt");
        try (FileWriter fw = new FileWriter(rep)) { fw.write(out.toString()); }
        println("WROTE " + rep.getAbsolutePath() + " (" + out.length() + " chars)");
    }

    void decompile(Function f, int depth, Set<Address> seen) {
        if (f == null || depth < 0) return;
        if (f.isExternal() || f.isThunk()) return;
        Address ep = f.getEntryPoint();
        if (!seen.add(ep)) return;
        long rva = ep.getOffset() - imageBase;
        long bytes = f.getBody().getNumAddresses();
        out.append("\n==== ").append(f.getName()).append(" @ ").append(ep)
           .append("  RVA 0x").append(Long.toHexString(rva))
           .append("  [bytes=").append(bytes).append("] ====\n");
        DecompileResults res = di.decompileFunction(f, 45, monitor);
        if (res != null && res.decompileCompleted())
            out.append(res.getDecompiledFunction().getC()).append("\n");
        else
            out.append("(decompile failed)\n");
        if (depth == 0) return;
        for (Function c : orderedCallees(f)) {
            long cb = c.getBody().getNumAddresses();
            if (cb > 0 && cb <= maxBytes) decompile(c, depth - 1, seen);
        }
    }

    LinkedHashSet<Function> orderedCallees(Function f) {
        LinkedHashSet<Function> out = new LinkedHashSet<>();
        InstructionIterator it = listing.getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType() != null && ins.getFlowType().isCall())
                for (Reference r : ins.getReferencesFrom()) {
                    Function t = fm.getFunctionAt(r.getToAddress());
                    if (t != null) out.add(t);
                }
        }
        return out;
    }
}
