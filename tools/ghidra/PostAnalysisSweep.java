// Fills undefined gaps in executable blocks after auto-analysis:
// walks every undefined, 4-aligned, non-zero word in initialized executable
// memory (main .text + overlay NAME.text blocks), tries to disassemble it and
// create a function there, then lets incremental analysis propagate (call
// targets, references). Repeats until no new code appears.
//
// Zero words are skipped (Metrowerks pads between functions with zeros, which
// would otherwise decode as nop runs).
// @category DQ8Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class PostAnalysisSweep extends GhidraScript {

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Listing listing = currentProgram.getListing();

        AddressSet execSet = new AddressSet();
        for (MemoryBlock blk : mem.getBlocks()) {
            if (blk.isExecute() && blk.isInitialized()) {
                execSet.addRange(blk.getStart(), blk.getEnd());
            }
        }

        int totalFuncs = 0;
        int totalDisassembled = 0;
        for (int round = 1; round <= 10 && !monitor.isCancelled(); round++) {
            int newCode = 0;
            int newFuncs = 0;
            AddressSetView undefined =
                listing.getUndefinedRanges(execSet, false, monitor);

            for (AddressRange range : undefined) {
                Address pos = range.getMinAddress();
                long misalign = pos.getOffset() & 3;
                if (misalign != 0) {
                    pos = pos.add(4 - misalign);
                }
                Address end = range.getMaxAddress();
                while (pos != null && pos.compareTo(end) < 0 && !monitor.isCancelled()) {
                    Instruction ins = getInstructionAt(pos);
                    if (ins != null) {
                        pos = ins.getMaxAddress().add(1);
                        continue;
                    }
                    if (listing.getDefinedDataContaining(pos) != null) {
                        pos = pos.add(4);
                        continue;
                    }
                    int w;
                    try {
                        w = mem.getInt(pos);
                    }
                    catch (Exception e) {
                        break;
                    }
                    if (w == 0) {
                        pos = pos.add(4);
                        continue;
                    }
                    disassemble(pos);
                    ins = getInstructionAt(pos);
                    if (ins == null) {
                        pos = pos.add(4);
                        continue;
                    }
                    newCode++;
                    if (getFunctionContaining(pos) == null && createFunction(pos, null) != null) {
                        newFuncs++;
                    }
                    pos = ins.getMaxAddress().add(1);
                }
            }

            totalFuncs += newFuncs;
            totalDisassembled += newCode;
            println("Sweep round " + round + ": disassembly seeds=" + newCode +
                    " new functions=" + newFuncs);
            if (newCode == 0) {
                break;
            }
            analyzeChanges(currentProgram);
        }
        println("PostAnalysisSweep done: " + totalDisassembled + " seeds, " +
                totalFuncs + " new functions created directly (more may come from analysis)");
    }
}
