import argparse
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "dq8_setup", Path(__file__).resolve().parents[2] / "setup.py")
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class ExtractionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.extracted = self.root / "disc"
        self.extracted.mkdir()
        for name in ("DATA.HD6", "DATA.DAT", "DATA2.HD6", "DATA2.DAT"):
            (self.extracted / name).touch()
        self.args = argparse.Namespace(iso=str(self.root / "disc.iso"),
                                      out=str(self.root / "out"),
                                      extracted=str(self.extracted), mvi_key=None)
        self.output = patch("sys.stdout", new_callable=io.StringIO)
        self.output.start()
        self.addCleanup(self.output.stop)

    def test_movie_key_required_before_any_extraction(self):
        (self.extracted / "MOVIE").mkdir()
        with patch.object(setup, "run_tool") as run, self.assertRaisesRegex(SystemExit, "--mvi-key"):
            setup.cmd_extract(self.args)
        run.assert_not_called()
        self.assertFalse(Path(self.args.out).exists())

    def test_bad_key_rejected_before_any_extraction(self):
        (self.extracted / "MOVIE").mkdir()
        key = self.root / "synthetic.key"
        key.write_bytes(b"too short")
        self.args.mvi_key = str(key)
        with patch.object(setup, "run_tool") as run, self.assertRaisesRegex(SystemExit, "2048-byte"):
            setup.cmd_extract(self.args)
        run.assert_not_called()

    def test_supplied_key_passed_to_movie_tool(self):
        (self.extracted / "MOVIE").mkdir()
        key = self.root / "synthetic.key"
        key.write_bytes(bytes(2048))
        self.args.mvi_key = str(key)
        with patch.object(setup, "run_tool") as run, patch("sys.stdout", new_callable=io.StringIO):
            setup.cmd_extract(self.args)
        self.assertEqual(run.call_count, 3)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("--key") + 1], str(key))
        self.assertEqual(command[command.index("--in") + 1], str(self.extracted / "MOVIE"))

    def test_tree_without_movies_needs_no_key(self):
        with patch.object(setup, "run_tool") as run, patch("sys.stdout", new_callable=io.StringIO):
            setup.cmd_extract(self.args)
        self.assertEqual(run.call_count, 2)

    def test_missing_archive_rejected_before_any_extraction(self):
        (self.extracted / "DATA2.DAT").unlink()
        with patch.object(setup, "run_tool") as run, self.assertRaisesRegex(SystemExit, "DATA2"):
            setup.cmd_extract(self.args)
        run.assert_not_called()
        self.assertFalse(Path(self.args.out).exists())


class VerificationTests(unittest.TestCase):
    def test_automatic_tree_matches_selected_region(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("Extracted_Usa", "Extracted_Eur"):
                (root / name).mkdir()
            for version, tree in setup.EXTRACTED_DIRS.items():
                with self.subTest(version=version):
                    args = argparse.Namespace(iso=str(root / "disc.iso"), version=version,
                                              extracted=None, all=False)
                    database = {"iso": {}, "elf": version, "files": {version: {}}}
                    with patch.object(setup, "load_hashes", return_value=database), \
                            patch.object(setup, "check_file", return_value=True) as check, \
                            patch("sys.stdout", new_callable=io.StringIO):
                        setup.cmd_verify(args)
                    self.assertEqual(check.call_args.args[0], root / tree / version)


if __name__ == "__main__":
    unittest.main()
