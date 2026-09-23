// Post-import fixup for the DQ8 main ELF (SLES_539.74 / SLUS_212.07).
// - Splits the single RWX LOAD block into .text (exec) / .data (no-exec) at the
//   boundary passed as script arg 1 (hex, e.g. 0x003dd3f0; computed by
//   run_analysis.sh as: last `jr ra` word + 8, rounded up to 16).
// - Creates the main .bss block and an OVERLAY_STAGE placeholder block (the
//   RAM region the overlays are streamed into at runtime); Ghidra's ELF
//   loader creates no blocks for the zero-filesize segments of this ELF.
//   Args 2..5 (all hex): <bssStart> <bssEnd> <ovlStart> <ovlEnd>; pass 0 0 to
//   skip either block.
// - Marks all other default-space blocks non-executable.
// - Converts the scratchpad-address table that sits AT the ELF entry point
//   (0x100008) into dword data so the entry-point analyzer does not
//   disassemble it, and seeds a function at the first real instruction after
//   the table (labelled crt0_start).
// @category DQ8Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ImportDQ8Main extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 5) {
            throw new IllegalArgumentException(
                "usage: ImportDQ8Main.java <textEnd> <bssStart> <bssEnd> <ovlStart> <ovlEnd> (hex)");
        }
        long textEnd = Long.decode(args[0]);
        long bssStart = Long.decode(args[1]);
        long bssEnd = Long.decode(args[2]);
        long ovlStart = Long.decode(args[3]);
        long ovlEnd = Long.decode(args[4]);

        Memory mem = currentProgram.getMemory();
        Address splitAddr = toAddr(textEnd);

        MemoryBlock mainBlk = mem.getBlock(toAddr(0x100000L));
        if (mainBlk == null) {
            throw new IllegalStateException("no memory block at 0x100000");
        }

        if (mainBlk.contains(splitAddr) && mainBlk.getStart().getOffset() < textEnd) {
            mem.split(mainBlk, splitAddr);
        }

        MemoryBlock textBlk = mem.getBlock(toAddr(0x100000L));
        textBlk.setName(".text");
        textBlk.setRead(true);
        textBlk.setWrite(false);
        textBlk.setExecute(true);

        for (MemoryBlock blk : mem.getBlocks()) {
            if (blk == textBlk || blk.getStart().getAddressSpace().isOverlaySpace()) {
                continue;
            }
            blk.setExecute(false);
            if (blk.getStart().getOffset() == textEnd && !blk.getName().startsWith(".")) {
                blk.setName(".data");
            }
        }
        println("ImportDQ8Main: .text = [0x100000, " + args[0] + "), rest of default space marked no-exec");

        if (bssEnd > bssStart && mem.getBlock(toAddr(bssStart)) == null) {
            MemoryBlock bss = mem.createUninitializedBlock(".bss", toAddr(bssStart),
                bssEnd - bssStart, false);
            bss.setRead(true);
            bss.setWrite(true);
            bss.setExecute(false);
            println(String.format("ImportDQ8Main: created .bss [0x%08X, 0x%08X)", bssStart, bssEnd));
        }
        if (ovlEnd > ovlStart && mem.getBlock(toAddr(ovlStart)) == null) {
            MemoryBlock stage = mem.createUninitializedBlock("OVERLAY_STAGE", toAddr(ovlStart),
                ovlEnd - ovlStart, false);
            stage.setRead(true);
            stage.setWrite(true);
            stage.setExecute(false);
            stage.setComment("Runtime destination of the MWo3 overlays (TITLE/BATTLE/...); " +
                "actual overlay contents are mapped as overlay blocks NAME.text/.data/.bss");
            println(String.format("ImportDQ8Main: created OVERLAY_STAGE [0x%08X, 0x%08X)",
                ovlStart, ovlEnd));
        }

        // ---- entry-point table handling -------------------------------------
        // The ELF entry (0x100008) points at a table of scratchpad addresses
        // (0x70000c28, 0x70001428, ... ascending) followed by a few small
        // config words (0x11, 0x70000011, 0x13, 0x70000013). Real crt0 code
        // starts right after it. Define the table as dwords and put a function
        // at the first non-table word.
        Address entry = toAddr(currentProgram.getImageBase().getOffset() == 0x100000L
                ? 0x100008L : 0x100008L);
        Address a = entry;
        int n = 0;
        while (n < 64) {
            int w = mem.getInt(a);
            long uw = w & 0xFFFFFFFFL;
            boolean tableWord = ((uw & 0xFF000000L) == 0x70000000L) || (uw < 0x100L);
            if (!tableWord) {
                break;
            }
            if (getDataAt(a) == null) {
                createDWord(a);
            }
            a = a.add(4);
            n++;
        }
        if (n > 0) {
            createLabel(entry, "crt0_scratchpad_table", true, SourceType.ANALYSIS);
            setPlateComment(entry,
                "ELF entry point lands here, but this is a table of scratchpad (0x70000000)\n" +
                "addresses + config words, not code. Real crt0 code starts at " + a + ".");
            println("ImportDQ8Main: defined " + n + " table dwords at entry " + entry +
                    ", real code seeded at " + a);
            disassemble(a);
            if (getFunctionAt(a) == null) {
                createFunction(a, "crt0_start");
            }
        }
        else {
            println("ImportDQ8Main: entry table not detected at " + entry + " (leaving as-is)");
        }
    }
}
