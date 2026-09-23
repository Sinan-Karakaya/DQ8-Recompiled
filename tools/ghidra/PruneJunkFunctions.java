// Deletes bogus functions that aggressive gap-sweeping creates when parts of
// an executable block are actually data (the DQ8 main ELF interleaves rodata
// stretches inside its single RWX segment). A function is considered junk if:
//   1. its entry is not in an initialized memory block (e.g. 1-byte stubs the
//      pointer analysis drops into the uninitialized OVERLAY_STAGE region), or
//   2. its body is smaller than 8 bytes (a real MIPS function is at least
//      jump + delay slot), or
//   3. it is an unnamed (FUN_*) function with no incoming references whose
//      body does not end like real code, i.e. the last instruction is neither
//      a jump/terminal flow nor the delay slot of one, and simply falls off
//      into undefined bytes. Real unreferenced functions (alignment orphans,
//      pointer-called code) always end in jr/j + delay slot and are kept.
// @category DQ8Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;

import java.util.ArrayList;
import java.util.List;

public class PruneJunkFunctions extends GhidraScript {

    @Override
    public void run() throws Exception {
        List<Function> doomed = new ArrayList<>();
        int uninit = 0;
        int tiny = 0;
        int noTerm = 0;

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            Address entry = f.getEntryPoint();
            MemoryBlock blk = currentProgram.getMemory().getBlock(entry);
            AddressSetView body = f.getBody();

            if (blk == null || !blk.isInitialized()) {
                doomed.add(f);
                uninit++;
                continue;
            }
            if (body == null || body.getNumAddresses() < 8) {
                doomed.add(f);
                tiny++;
                continue;
            }
            if (!f.getName().startsWith("FUN_")) {
                continue;
            }
            if (currentProgram.getReferenceManager().getReferencesTo(entry).hasNext()) {
                continue;
            }
            Instruction last =
                currentProgram.getListing().getInstructionContaining(body.getMaxAddress());
            boolean goodEnding = false;
            if (last != null) {
                FlowType ft = last.getFlowType();
                goodEnding = last.isInDelaySlot() || ft.isTerminal() || ft.isJump();
            }
            if (!goodEnding) {
                doomed.add(f);
                noTerm++;
            }
        }

        for (Function f : doomed) {
            removeFunction(f);
        }
        println("PruneJunkFunctions: removed " + doomed.size() + " functions (" +
            uninit + " in uninitialized memory, " + tiny + " under 8 bytes, " +
            noTerm + " unreferenced with no code-like ending)");
    }
}
