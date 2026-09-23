// Seeds function names from known strings in the DQ8 binaries.
// The game has an empty .symtab, but assert/log format strings reveal source
// file basenames and some class methods. For each known string, this script
// finds all occurrences in initialized memory (main image + overlay blocks),
// then renames every still-default (FUN_*) function that references the
// string, e.g. FUN_00212345 -> dq_monster_ai_cpp__assert_ref_1 or
// CPutMonster__Revive_logref_1. Functions that already have a name only get
// the derived name added as an extra label.
// @category DQ8Recomp
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.SourceType;

public class ApplyKnownNames extends GhidraScript {

    // string -> label stem
    private static final Map<String, String> KNOWN = new LinkedHashMap<>();
    static {
        String[] assertFiles = {
            "mg_draw.cpp", "dq_2nd_chk.cpp", "dq_monster_ai.cpp", "dq_monster_ai.h",
            "btl_playdata.cpp", "dq_player_ai.cpp", "playerai_data.h", "playerai_data.cpp",
            "btl_actana.cpp", "btl_viscom.cpp", "btl_chrinfo_logic.cpp", "eff_hdl.h"
        };
        for (String f : assertFiles) {
            KNOWN.put(f, f.replace('.', '_') + "__assert_ref");
        }
        String[] methods = {
            "CPutMonster::Dead", "CPutMonster::Insert", "CPutMonster::Revive",
            "CBuildMonster::Regist", "CBuildMonster::GetChara",
            "CBtlPCParts::Create", "BTL_CHARA_INFO::SetNowBaseMotType"
        };
        for (String m : methods) {
            KNOWN.put(m, m.replace("::", "__") + "_logref");
        }
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        int renamed = 0;
        int labeled = 0;

        for (Map.Entry<String, String> e : KNOWN.entrySet()) {
            byte[] needle = e.getKey().getBytes(StandardCharsets.US_ASCII);
            String stem = e.getValue();
            Set<Function> targets = new LinkedHashSet<>();
            int hits = 0;

            for (MemoryBlock blk : mem.getBlocks()) {
                if (!blk.isInitialized()) {
                    continue;
                }
                Address pos = blk.getStart();
                while (pos != null && pos.compareTo(blk.getEnd()) <= 0) {
                    Address found = mem.findBytes(pos, blk.getEnd(), needle, null, true, monitor);
                    if (found == null) {
                        break;
                    }
                    hits++;
                    collectReferencingFunctions(found, targets);
                    Data d = currentProgram.getListing().getDefinedDataContaining(found);
                    if (d != null && !d.getAddress().equals(found)) {
                        collectReferencingFunctions(d.getAddress(), targets);
                    }
                    pos = found.add(1);
                }
            }

            int i = 0;
            for (Function fn : targets) {
                i++;
                String name = stem + "_" + i;
                if (fn.getName().startsWith("FUN_")) {
                    fn.setName(name, SourceType.ANALYSIS);
                    renamed++;
                }
                else {
                    createLabel(fn.getEntryPoint(), name, false, SourceType.ANALYSIS);
                    labeled++;
                }
            }
            println(String.format("'%s': %d occurrence(s), %d referencing function(s)",
                e.getKey(), hits, targets.size()));
        }
        println("ApplyKnownNames: renamed " + renamed + " functions, added " + labeled +
                " extra labels");
    }

    private void collectReferencingFunctions(Address strAddr, Set<Function> out) {
        ReferenceIterator refs =
            currentProgram.getReferenceManager().getReferencesTo(strAddr);
        while (refs.hasNext()) {
            Reference r = refs.next();
            Function fn = getFunctionContaining(r.getFromAddress());
            if (fn != null) {
                out.add(fn);
            }
        }
    }
}
