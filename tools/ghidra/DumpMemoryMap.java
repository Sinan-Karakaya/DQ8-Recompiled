// Prints the program's memory map (space, range, perms, init) — debugging aid.
// @category DQ8Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class DumpMemoryMap extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(String.format("%-16s space=%-14s [%s, %s] size=0x%-8X %s%s%s %s",
                b.getName(), b.getStart().getAddressSpace().getName(),
                b.getStart(), b.getEnd(), b.getSize(),
                b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-",
                b.isInitialized() ? "init" : "uninit"));
        }
    }
}
