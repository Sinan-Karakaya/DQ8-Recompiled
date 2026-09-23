import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "source_check", Path(__file__).resolve().parents[1] / "check_source_distribution.py")
source_check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(source_check)


class SourceDistributionTests(unittest.TestCase):
    def test_known_payload_names(self):
        for name in ("disc.ISO", "local/SLUS_212.07", "overlays/title.elf",
                     "tools/mvi/mvi_xor_key.bin", "frame.gs.zst", "vu.state",
                     "build/generated/source.cpp", "Assets_Usa/font.img",
                     "generated/FUN_00100000.cpp", "src/FUN_00100000.cpp",
                     "src/FUN_00145080_0x145080.cpp",
                     "private.pem", ".env", ".env.local"):
            with self.subTest(name=name):
                self.assertIsNotNone(source_check.path_violation(name))

    def test_source_and_metadata_names(self):
        for name in ("config/SLUS_212.07/hashes.json", "config/SLES_539.74/functions.csv",
                     "tools/mvi/mvi_decrypt.py", "tools/runtime/vu_math_main.cpp",
                     "src/gfx/shaders/gs.frag.hlsl", ".env.example"):
            with self.subTest(name=name):
                self.assertIsNone(source_check.path_violation(name))

    def test_tracked_files_and_secret_detection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "safe.cpp").write_text("// authored fixture\n")
            (root / "hashes.json").write_text('{"sha256":"' + "a" * 64 + '"}\n')
            (root / "untracked.iso").write_bytes(b"synthetic")
            subprocess.run(["git", "-C", str(root), "add", "safe.cpp", "hashes.json"], check=True)
            subprocess.run(["git", "-C", str(root), "update-index", "--add", "--cacheinfo",
                            "160000," + "1" * 40 + ",thirdparty/authored-fixtures"], check=True)
            self.assertEqual(source_check.check_checkout(root), [])
            (root / "private.txt").write_text("-----BEGIN " + "PRIVATE KEY-----\nsynthetic\n")
            (root / "tracked.iso").write_bytes(b"synthetic")
            subprocess.run(["git", "-C", str(root), "add", "private.txt", "tracked.iso"], check=True)
            self.assertEqual(dict(source_check.check_checkout(root)), {
                "private.txt": "credential signature",
                "tracked.iso": "game binary, archive, or capture",
            })


if __name__ == "__main__":
    unittest.main()
