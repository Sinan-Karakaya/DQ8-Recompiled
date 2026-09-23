import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ghidra/run_analysis.sh"


class GhidraLauncherTests(unittest.TestCase):
    def test_unknown_stage_fails_before_configuration(self):
        result = subprocess.run(["bash", str(SCRIPT), "unknown"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    def test_missing_explicit_inputs_fail(self):
        environment = {k: v for k, v in os.environ.items() if k not in {"GHIDRA_HOME", "ELF"}}
        result = subprocess.run(["bash", str(SCRIPT), "post"], env=environment,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("set GHIDRA_HOME", result.stderr)

    def test_import_and_export_keep_paths_and_region_overlays(self):
        with tempfile.TemporaryDirectory(prefix="dq8 tools ") as directory:
            root = Path(directory)
            launcher = root / "ghidra/support/analyzeHeadless"
            launcher.parent.mkdir(parents=True)
            launcher.write_text(
                "#!/usr/bin/env python3\nimport json, os, sys\n"
                "with open(os.environ['COMMAND_LOG'], 'a') as log:\n"
                "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n")
            launcher.chmod(0o755)
            javac = root / "jdk/bin/javac"
            javac.parent.mkdir(parents=True)
            javac.write_text("#!/bin/sh\nexit 0\n")
            javac.chmod(0o755)
            disc = root / "disc"
            (disc / "BIN").mkdir(parents=True)
            # Authored ELF layout: one JR+delay pair and an empty overlay segment.
            blob = bytearray(0x88)
            struct.pack_into("<I", blob, 0x1C, 0x34)
            struct.pack_into("<H", blob, 0x2C, 2)
            struct.pack_into("<8I", blob, 0x34, 1, 0x80, 0x100000, 0, 8, 24, 0, 0)
            struct.pack_into("<8I", blob, 0x54, 1, 0, 0x200000, 0, 0, 0x1000, 0, 0)
            struct.pack_into("<2I", blob, 0x80, 0x03E00008, 0)
            elf = disc / "SLUS_212.07"
            elf.write_bytes(blob)
            environment = {**os.environ, "GHIDRA_HOME": str(root / "ghidra"),
                           "JAVA_HOME": str(root / "jdk"), "ELF": str(elf),
                           "PROJECT_DIR": str(root / "project"), "OUT_DIR": str(root / "csv"),
                           "COMMAND_LOG": str(root / "commands.jsonl")}
            environment.pop("OVERLAYS", None)
            environment.pop("BIN_DIR", None)
            result = subprocess.run(["bash", str(SCRIPT), "all"], env=environment,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = [json.loads(line) for line in (root / "commands.jsonl").read_text().splitlines()]
            self.assertEqual(len(commands), 3)
            self.assertIn(str(elf), commands[0])
            self.assertIn(str(disc / "BIN"), commands[0])
            self.assertIn("0x00100010", commands[0])
            self.assertIn("VIEWER", commands[0])
            self.assertIn("VIEWER", commands[2])


if __name__ == "__main__":
    unittest.main()
