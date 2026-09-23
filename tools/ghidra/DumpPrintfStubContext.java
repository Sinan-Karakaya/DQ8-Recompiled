// Oracle task 1 support script: independently derive/cross-check the two
// stubbed-printf re-enable addresses reported by TCRF for SLUS_212.07.
//
// Prints, for each address of interest, the containing function's full
// EE-aware disassembly (r5900 MMI-correct, unlike a plain MIPS32 decoder)
// and decompiled C, plus every caller (incoming reference) of the shared
// low-level log function. Run with -noanalysis against the already
// analyzed DQ8U project (main ELF only, program SLUS_212.07):
//
//   analyzeHeadless <PROJECT_DIR> DQ8U -process SLUS_212.07 -noanalysis \
//       -scriptPath tools/ghidra -postScript DumpPrintfStubContext.java \
//       -log logs/oracle_printf.log
//
// See https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing for how these addresses were
// cross-checked against TCRF's published Action Replay/CodeBreaker codes.
// @category DQ8Recomp.Oracle
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.task.ConsoleTaskMonitor;

public class DumpPrintfStubContext extends GhidraScript {

    // Addresses of interest in the main ELF address space.
    private static final long[] TARGETS = new long[] {
        0x00146E20L, // general printf() wrapper (TCRF: "Re-enable printf")
        0x00321620L, // battle debug logger wrapper (TCRF: "...In Battle")
        0x00119508L, // candidate shared low-level log/output function
        0x00115BA0L, // MPEG-error call site, believed to already call it live
    };

    private void dumpDisasm(Function fn) throws Exception {
        InstructionIterator it = currentProgram.getListing()
            .getInstructions(fn.getBody(), true);
        while (it.hasNext()) {
            Instruction insn = it.next();
            println(String.format("    %-12s %s", insn.getAddress(), insn.toString()));
        }
    }

    private void dumpDecompiled(Function fn, DecompInterface decomp) {
        DecompileResults res = decomp.decompileFunction(fn, 60, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted()) {
            println("  -- decompiled --");
            println(res.getDecompiledFunction().getC());
        } else {
            println("  -- decompile failed: " +
                (res != null ? res.getErrorMessage() : "null result") + " --");
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        for (long a : TARGETS) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
            Function fn = currentProgram.getFunctionManager().getFunctionContaining(addr);
            println("\n================ 0x" + Long.toHexString(a) + " ================");
            if (fn == null) {
                println("  no containing function (address may be mid-gap or data)");
                continue;
            }
            println("  function: " + fn.getName() + "  body=[" + fn.getBody().getMinAddress()
                + ", " + fn.getBody().getMaxAddress() + "]");
            println("  -- disassembly --");
            dumpDisasm(fn);
            dumpDecompiled(fn, decomp);
        }

        // Every caller of the candidate shared log function, to prove (or
        // disprove) that it is already live/used elsewhere in retail.
        Address logAddr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x00119508L);
        println("\n================ references TO 0x00119508 ================");
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(logAddr);
        int n = 0;
        while (refs.hasNext()) {
            Reference r = refs.next();
            println("  from " + r.getFromAddress() + "  type=" + r.getReferenceType());
            n++;
        }
        println("  total: " + n);

        decomp.dispose();
    }
}
