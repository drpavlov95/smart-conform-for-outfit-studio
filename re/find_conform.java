// Ghidra GhidraScript (Java) — locate the conform call chain in OutfitStudio x64.exe.
// Anchors on unique strings in OutfitStudioFrame::ConformShapes and dumps a call
// tree so we can identify ConformShape -> Automorph::GenerateResultDiff and
// Automorph::ZeroVertDiff. Writes drape_re_report.txt (DRAPE_RE_OUT env).
//
// @category Drape

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import java.io.File;
import java.io.FileWriter;
import java.util.*;

public class find_conform extends GhidraScript {
    StringBuilder out = new StringBuilder();
    void emit(String s) { out.append(s).append("\n"); println(s); }

    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        emit("imagebase: " + currentProgram.getImageBase());

        String[] anchors = {"All shapes conformed.", "Conforming shapes...",
                            "dlgConforming", "proximityRadiusSlider"};
        Set<Function> roots = new LinkedHashSet<>();

        DataIterator di = listing.getDefinedData(true);
        while (di.hasNext()) {
            Data d = di.next();
            if (d == null) continue;
            DataType dt = d.getDataType();
            if (dt == null) continue;
            String tn = dt.getName().toLowerCase();
            if (!(tn.contains("string") || tn.contains("unicode") || tn.contains("char")))
                continue;
            Object v = d.getValue();
            if (v == null) continue;
            String sv = v.toString();
            for (String a : anchors) {
                if (sv.contains(a)) {
                    emit("\nstring '" + a + "' @ " + d.getAddress());
                    for (Reference r : rm.getReferencesTo(d.getAddress())) {
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        if (f != null) {
                            emit("  ref by " + f.getName() + " @ " + f.getEntryPoint());
                            roots.add(f);
                        }
                    }
                }
            }
        }

        emit("\n=== CALL TREE (depth 4) ===");
        for (Function f : roots) {
            emit("\n>>> root " + f.getName() + " @ " + f.getEntryPoint());
            dump(f, 4, new HashSet<Address>(), "  ", listing, fm);
        }

        String outDir = System.getenv("DRAPE_RE_OUT");
        if (outDir == null) outDir = ".";
        File rep = new File(outDir, "drape_re_report.txt");
        try (FileWriter fw = new FileWriter(rep)) { fw.write(out.toString()); }
        println("WROTE " + rep.getAbsolutePath());
    }

    void dump(Function f, int depth, Set<Address> seen, String prefix,
              Listing listing, FunctionManager fm) {
        if (f == null || depth < 0) return;
        Address ep = f.getEntryPoint();
        int nCalls = countCalls(f, listing, fm);
        long bodyLen = f.getBody().getNumAddresses();
        if (seen.contains(ep)) {
            emit(prefix + f.getName() + " @ " + ep + "  (revisited)");
            return;
        }
        seen.add(ep);
        emit(prefix + f.getName() + " @ " + ep + "  [bytes=" + bodyLen +
             " calls=" + nCalls + "]");
        if (depth == 0) return;
        for (Function c : orderedCallees(f, listing, fm))
            dump(c, depth - 1, seen, prefix + "    ", listing, fm);
    }

    LinkedHashSet<Function> orderedCallees(Function f, Listing listing, FunctionManager fm) {
        LinkedHashSet<Function> out = new LinkedHashSet<>();
        InstructionIterator it = listing.getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType() != null && ins.getFlowType().isCall()) {
                for (Reference r : ins.getReferencesFrom()) {
                    Function t = fm.getFunctionAt(r.getToAddress());
                    if (t != null) out.add(t);
                }
            }
        }
        return out;
    }

    int countCalls(Function f, Listing listing, FunctionManager fm) {
        int n = 0;
        InstructionIterator it = listing.getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType() != null && ins.getFlowType().isCall()) n++;
        }
        return n;
    }
}
