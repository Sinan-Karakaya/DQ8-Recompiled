// Imports the DQ8 Metrowerks MWo3 overlay binaries into the CURRENT program
// (the main ELF) as Ghidra overlay memory blocks, so overlay code can resolve
// calls/data references into the main executable image.
//
// MWo3 file layout: 0x40-byte header { char magic[4]="MWo3"; u32 id;
// u32 load_addr; u32 text_size; u32 data_size; u32 bss_size; u32 ctor_begin;
// u32 ctor_end; char name[32]; }, followed by text_size bytes of .text and
// data_size bytes of .data. The header itself is loaded at load_addr, so
// .text lives at load_addr+0x40.
//
// For each overlay this script creates NAME.text (exec) / NAME.data / NAME.bss
// blocks in one overlay address space, then seeds analysis with the overlay
// entry (start of .text) and every static-constructor pointer from the ctor
// table. References to addresses outside the overlay blocks fall through to
// the default address space, i.e. into the main ELF image.
//
// Script args: <binDir> [NAME ...]   (default names: TITLE BATTLE MENU SHOP CASINO)
// @category DQ8Recomp
import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.OverlayAddressSpace;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ImportDQ8Overlays extends GhidraScript {

    private static final String[] DEFAULT_NAMES = { "TITLE", "BATTLE", "MENU", "SHOP", "CASINO" };

    private static long le32(byte[] b, int off) {
        return (b[off] & 0xFFL) | ((b[off + 1] & 0xFFL) << 8) |
               ((b[off + 2] & 0xFFL) << 16) | ((b[off + 3] & 0xFFL) << 24);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ImportDQ8Overlays.java <binDir> [NAME ...]");
        }
        File dir = new File(args[0]);
        String[] names = args.length > 1 ? Arrays.copyOfRange(args, 1, args.length) : DEFAULT_NAMES;
        for (String name : names) {
            importOverlay(new File(dir, name + ".BIN"), name);
        }
    }

    private void importOverlay(File f, String name) throws Exception {
        byte[] all = Files.readAllBytes(f.toPath());
        if (all.length < 0x40 || all[0] != 'M' || all[1] != 'W' || all[2] != 'o' || all[3] != '3') {
            throw new IllegalStateException(f + ": not an MWo3 overlay");
        }
        long id = le32(all, 0x04);
        long loadAddr = le32(all, 0x08);
        int textSize = (int) le32(all, 0x0C);
        int dataSize = (int) le32(all, 0x10);
        int bssSize = (int) le32(all, 0x14);
        long ctorBegin = le32(all, 0x18);
        long ctorEnd = le32(all, 0x1C);
        long textStart = loadAddr + 0x40;

        println(String.format(
            "Overlay %s: id=%d load=0x%08X text=0x%X data=0x%X bss=0x%X ctors=[0x%08X,0x%08X)",
            name, id, loadAddr, textSize, dataSize, bssSize, ctorBegin, ctorEnd));

        Memory mem = currentProgram.getMemory();
        byte[] textBytes = Arrays.copyOfRange(all, 0x40, 0x40 + textSize);

        MemoryBlock textBlk = mem.createInitializedBlock(name + ".text", toAddr(textStart),
            new ByteArrayInputStream(textBytes), textSize, monitor, true);
        textBlk.setRead(true);
        textBlk.setWrite(false);
        textBlk.setExecute(true);

        // NOTE: must use getAddressInThisSpaceOnly() — plain getAddress() on an
        // overlay space silently falls through to the base (ram) space for
        // offsets outside the blocks already defined in the overlay.
        OverlayAddressSpace ovl = (OverlayAddressSpace) textBlk.getStart().getAddressSpace();

        if (dataSize > 0) {
            byte[] dataBytes = Arrays.copyOfRange(all, 0x40 + textSize, 0x40 + textSize + dataSize);
            Address dataStart = ovl.getAddressInThisSpaceOnly(textStart + textSize);
            MemoryBlock dataBlk = mem.createInitializedBlock(name + ".data", dataStart,
                new ByteArrayInputStream(dataBytes), dataSize, monitor, false);
            dataBlk.setRead(true);
            dataBlk.setWrite(true);
            dataBlk.setExecute(false);
        }
        if (bssSize > 0) {
            Address bssStart = ovl.getAddressInThisSpaceOnly(textStart + textSize + dataSize);
            MemoryBlock bssBlk = mem.createUninitializedBlock(name + ".bss", bssStart, bssSize, false);
            bssBlk.setRead(true);
            bssBlk.setWrite(true);
            bssBlk.setExecute(false);
        }

        // Seed: overlay entry = start of .text.
        Address entry = ovl.getAddressInThisSpaceOnly(textStart);
        disassemble(entry);
        if (getFunctionAt(entry) == null) {
            createFunction(entry, name.toLowerCase() + "_overlay_entry");
        }
        else {
            createLabel(entry, name.toLowerCase() + "_overlay_entry", true, SourceType.ANALYSIS);
        }

        // Seed: static-constructor table entries. NOTE: in DQ8's overlays these
        // point into the .data region (Metrowerks emits the static-init
        // trampolines there), so accept any target inside text+data.
        int seeded = 0;
        for (long c = ctorBegin; c + 4 <= ctorEnd; c += 4) {
            long fileOff = 0x40 + (c - textStart);
            if (fileOff < 0x40 || fileOff + 4 > all.length) {
                continue;
            }
            long p = le32(all, (int) fileOff);
            if (p >= textStart && p < textStart + textSize + dataSize && (p & 3) == 0) {
                Address t = ovl.getAddressInThisSpaceOnly(p);
                disassemble(t);
                if (getFunctionAt(t) == null && createFunction(t, null) != null) {
                    seeded++;
                }
            }
        }
        println("Overlay " + name + ": blocks created in space '" + ovl.getName() +
                "', ctor functions seeded: " + seeded);
    }
}
