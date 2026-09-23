// Exports PS2Recomp-compatible function-boundary CSVs from the combined
// DQ8 program (main ELF + overlay blocks), one CSV per address space:
//   functions.csv            <- default space (main ELF)
//   functions_<overlay>.csv  <- each overlay space (TITLE.text, BATTLE.text, ...)
//
// CSV format matches thirdparty/PS2Recomp .../tools/ghidra/ExportPS2Functions.java:
//   header "Name,Start,End,Size"; Start/End are 0x%08X hex, End is exclusive
//   (body max address + 1), Size is decimal byte count of the function body.
//
// Also prints sanity stats per unit: function count, first/last function,
// covered bytes vs .text size, and undefined .text gaps > 16 bytes.
//
// Script args: <outDir> [NAME ...]   (default names: TITLE BATTLE MENU SHOP CASINO)
// @category DQ8Recomp
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;

public class ExportDQ8Csvs extends GhidraScript {

    private static final String[] DEFAULT_NAMES = { "TITLE", "BATTLE", "MENU", "SHOP", "CASINO" };

    private static final class Rec {
        String name;
        long start;
        long end;
        long size;
        AddressSetView body;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportDQ8Csvs.java <outDir> [NAME ...]");
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        String[] overlayNames =
            args.length > 1 ? Arrays.copyOfRange(args, 1, args.length) : DEFAULT_NAMES;

        List<Rec> main = new ArrayList<>();
        List<List<Rec>> perOverlay = new ArrayList<>();
        for (int i = 0; i < overlayNames.length; i++) {
            perOverlay.add(new ArrayList<>());
        }

        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function fn = it.next();
            AddressSetView body = fn.getBody();
            if (body == null || body.getNumAddresses() == 0) {
                continue;
            }
            Rec r = new Rec();
            // sanitize to C-identifier-safe names (overlay defaults look like
            // FUN_VIEWER.text__00461d30 — the dot would leak into generated code)
            r.name = fn.getName().replace(',', '_').replace(':', '_')
                                 .replace(' ', '_').replace('.', '_');
            r.start = fn.getEntryPoint().getOffset();
            r.end = body.getMaxAddress().getOffset() + 1L;
            r.size = body.getNumAddresses();
            r.body = body;
            // Non-contiguous bodies (fragmented flow, shared tails) would make
            // [Start,End) ranges overlap other functions; clamp such rows to
            // the entry fragment so every exported range is contiguous.
            if (r.end - r.start != r.size) {
                r.end = body.getFirstRange().getMaxAddress().getOffset() + 1L;
                r.size = r.end - r.start;
            }

            String space = fn.getEntryPoint().getAddressSpace().getName();
            if (!fn.getEntryPoint().getAddressSpace().isOverlaySpace()) {
                main.add(r);
            }
            else {
                for (int i = 0; i < overlayNames.length; i++) {
                    if (space.toUpperCase().startsWith(overlayNames[i].toUpperCase())) {
                        perOverlay.get(i).add(r);
                        break;
                    }
                }
            }
        }

        writeCsv(new File(outDir, "functions.csv"), main);
        stats("MAIN", main, findBlock(".text", null));
        for (int i = 0; i < overlayNames.length; i++) {
            String n = overlayNames[i];
            writeCsv(new File(outDir, "functions_" + n.toLowerCase() + ".csv"), perOverlay.get(i));
            stats(n, perOverlay.get(i), findBlock(n + ".text", n));
        }
    }

    private MemoryBlock findBlock(String name, String prefix) {
        for (MemoryBlock blk : currentProgram.getMemory().getBlocks()) {
            if (blk.getName().equals(name)) {
                return blk;
            }
        }
        if (prefix != null) {
            for (MemoryBlock blk : currentProgram.getMemory().getBlocks()) {
                if (blk.getName().toUpperCase().startsWith(prefix.toUpperCase()) && blk.isExecute()) {
                    return blk;
                }
            }
        }
        return null;
    }

    private void writeCsv(File f, List<Rec> recs) throws Exception {
        recs.sort(Comparator.comparingLong(r -> r.start));
        java.util.Set<String> seen = new java.util.HashSet<>();
        try (PrintWriter w = new PrintWriter(f)) {
            w.println("Name,Start,End,Size");
            for (Rec r : recs) {
                String name = r.name;
                if (!seen.add(name)) {
                    // duplicate display names (e.g. several thunk_FUN_xxx to
                    // the same target) -> make unique per file
                    name = String.format("%s_%08x", name, r.start);
                    seen.add(name);
                }
                w.printf("%s,0x%08X,0x%08X,%d%n", name, r.start, r.end, r.size);
            }
        }
        println("Wrote " + recs.size() + " functions to " + f.getAbsolutePath());
    }

    private void stats(String unit, List<Rec> recs, MemoryBlock textBlk) {
        if (recs.isEmpty()) {
            println("STATS " + unit + ": no functions!");
            return;
        }
        long covered = 0;
        AddressSet bodies = new AddressSet();
        for (Rec r : recs) {
            covered += r.size;
            bodies.add(r.body);
        }
        Rec first = recs.get(0);
        Rec last = recs.get(recs.size() - 1);
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("STATS %s: functions=%d first=0x%08X last=[0x%08X,0x%08X) coveredBytes=0x%X",
            unit, recs.size(), first.start, last.start, last.end, covered));
        if (textBlk != null) {
            long textSize = textBlk.getSize();
            AddressSet text = new AddressSet(textBlk.getStart(), textBlk.getEnd());
            AddressSetView holes = text.subtract(bodies);
            int bigGaps = 0;
            long holeBytes = 0;
            long biggest = 0;
            long biggestAt = 0;
            for (AddressRange range : holes) {
                long len = range.getLength();
                holeBytes += len;
                if (len > 16) {
                    bigGaps++;
                }
                if (len > biggest) {
                    biggest = len;
                    biggestAt = range.getMinAddress().getOffset();
                }
            }
            sb.append(String.format(" textSize=0x%X textUncovered=0x%X gapsOver16=%d biggestGap=0x%X@0x%08X",
                textSize, holeBytes, bigGaps, biggest, biggestAt));
        }
        println(sb.toString());
    }
}
