import csv
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ghidra/fix_truncated_boundaries.py"
BASE = 0x100000
NOP, JR_RA = 0x00000000, 0x03E00008


def branch(op: int, rs: int, rt: int, at: int, target: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (((target - at - 4) >> 2) & 0xFFFF)


class OutlinedTailTests(unittest.TestCase):
    def run_tool(self, words: list[int], rows: list[tuple[str, int, int]]):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = b"".join(struct.pack("<I", word) for word in words)
            header = bytearray(0x80)
            header[:4] = b"\x7fELF"
            struct.pack_into("<I", header, 0x1C, 0x34)
            struct.pack_into("<HH", header, 0x2A, 32, 1)
            struct.pack_into("<8I", header, 0x34, 1, 0x80, BASE, BASE, len(code), len(code), 7, 0x80)
            elf = root / "test.elf"
            elf.write_bytes(bytes(header) + code)
            table = root / "functions.csv"
            with table.open("w", newline="") as handle:
                writer = csv.writer(handle, lineterminator="\n")
                writer.writerow(["Name", "Start", "End", "Size"])
                for name, start, end in rows:
                    writer.writerow([name, f"0x{start:08X}", f"0x{end:08X}", end - start])
            result = subprocess.run([sys.executable, str(SCRIPT), "--elf", str(elf), "--csv", str(table),
                                     "--in-place"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with table.open(newline="") as handle:
                ends = {row["Name"]: int(row["End"], 16) for row in csv.DictReader(handle)}
            return ends, result.stderr

    def test_tail_needs_a_way_in_as_well_as_a_way_back(self):
        words = [
            # 0x100000 F: branches out to its cold block, which branches back.
            branch(0x04, 4, 0, BASE + 0x00, BASE + 0x30), NOP, JR_RA, NOP,
            # 0x100010 G, between F and the tail.
            JR_RA, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
            # 0x100030 unmapped: F's cold block.
            0x24020001, branch(0x04, 0, 0, BASE + 0x34, BASE + 0x08), NOP, NOP,
            # 0x100040 H.
            JR_RA, NOP,
            # 0x100048 unmapped data whose first word reads as `b` into F,
            # like the DMA channel addresses 0x10008000, 0x10009000, ...
            branch(0x04, 0, 0, BASE + 0x48, BASE + 0x04), 0x10008000,
            # 0x100050 I.
            JR_RA, NOP,
        ]
        rows = [("F", BASE, BASE + 0x10), ("G", BASE + 0x10, BASE + 0x30),
                ("H", BASE + 0x40, BASE + 0x48), ("I", BASE + 0x50, BASE + 0x58)]
        ends, stderr = self.run_tool(words, rows)
        self.assertEqual(ends["F"], BASE + 0x40)
        self.assertEqual(ends["G"], BASE + 0x30)
        self.assertEqual(ends["H"], BASE + 0x48)
        self.assertIn("UNREACHED F 0x00100000: 0x00100048-0x00100050", stderr)

    def test_tail_reached_through_another_tail(self):
        words = [
            # 0x100000 F enters the first block only; that one leads to the second.
            branch(0x04, 4, 0, BASE + 0x00, BASE + 0x18), NOP, JR_RA, NOP,
            # 0x100010 G.
            JR_RA, NOP,
            # 0x100018 unmapped: first block, back into F and on to the second.
            branch(0x05, 2, 0, BASE + 0x18, BASE + 0x30), NOP,
            branch(0x04, 0, 0, BASE + 0x20, BASE + 0x08), NOP,
            # 0x100028 H.
            JR_RA, NOP,
            # 0x100030 unmapped: second block, back into the first.
            branch(0x04, 0, 0, BASE + 0x30, BASE + 0x20), NOP,
            # 0x100038 I.
            JR_RA, NOP,
        ]
        rows = [("F", BASE, BASE + 0x10), ("G", BASE + 0x10, BASE + 0x18),
                ("H", BASE + 0x28, BASE + 0x30), ("I", BASE + 0x38, BASE + 0x40)]
        ends, _ = self.run_tool(words, rows)
        self.assertEqual(ends["F"], BASE + 0x38)

    def test_a_call_is_no_way_in(self):
        # `bgezal $zero` (bal) calls like `jal`, so the block is a callee even if it branches back.
        jal = (0x03 << 26) | ((BASE + 0x30) >> 2)
        bal = branch(0x01, 0, 0x11, BASE + 0x00, BASE + 0x30)
        for name, call in (("jal", jal), ("bgezal", bal)):
            with self.subTest(call=name):
                words = [
                    call, NOP, JR_RA, NOP,                                  # 0x100000 F
                    JR_RA, NOP, NOP, NOP, NOP, NOP, NOP, NOP,               # 0x100010 G
                    0x24020001, branch(0x04, 0, 0, BASE + 0x34, BASE + 0x08), NOP, NOP,
                    JR_RA, NOP,                                             # 0x100040 H
                ]
                rows = [("F", BASE, BASE + 0x10), ("G", BASE + 0x10, BASE + 0x30),
                        ("H", BASE + 0x40, BASE + 0x48)]
                ends, stderr = self.run_tool(words, rows)
                self.assertEqual(ends["F"], BASE + 0x10)
                self.assertIn("UNREACHED F 0x00100000: 0x00100030-0x00100040", stderr)

    def test_a_call_is_no_way_back(self):
        words = [
            branch(0x04, 4, 0, BASE + 0x00, BASE + 0x30), NOP, JR_RA, NOP,  # 0x100000 F
            JR_RA, NOP, NOP, NOP, NOP, NOP, NOP, NOP,                       # 0x100010 G
            # 0x100030 unmapped: only a `bgezal` leads into F.
            branch(0x01, 0, 0x11, BASE + 0x30, BASE + 0x08), NOP, JR_RA, NOP,
            JR_RA, NOP,                                                     # 0x100040 H
        ]
        rows = [("F", BASE, BASE + 0x10), ("G", BASE + 0x10, BASE + 0x30),
                ("H", BASE + 0x40, BASE + 0x48)]
        ends, _ = self.run_tool(words, rows)
        self.assertEqual(ends["F"], BASE + 0x10)


if __name__ == "__main__":
    unittest.main()
