import argparse
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10
    tomllib = None


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


@unittest.skipIf(tomllib is None, "tomllib needs Python 3.11")
class RecompileTests(unittest.TestCase):
    def recompile(self, repo: Path, extracted: Path):
        """The main config's [general] table and ps2_recomp's call, with no tool run."""
        (extracted / "BIN").mkdir(parents=True)
        (extracted / "SLUS_212.07").touch()
        for overlay in ("TITLE", "CASINO", "VIEWER", "BATTLE", "MENU", "SHOP"):
            (extracted / "BIN" / f"{overlay}.BIN").touch()
        repo.mkdir(parents=True)
        (repo / "ps2_recomp").touch()
        args = argparse.Namespace(version="SLUS_212.07", extracted=str(extracted),
                                  recompiler=str(repo / "ps2_recomp"))
        with patch.object(setup, "REPO_ROOT", repo), patch.object(setup.subprocess, "run") as run:
            setup.cmd_recompile(args)
        call = run.call_args_list[0]
        config = (repo / call.args[0][1]).read_text(encoding="utf-8")
        return tomllib.loads(config)["general"], call

    def test_launcher_layout_leaves_the_user_folder_out(self):
        # The launcher unpacks the source beside the disc's files, both under
        # the player's folder.
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory).resolve() / "Jo\N{LATIN SMALL LETTER A WITH TILDE}o" / "DQ8Recomp"
            general, call = self.recompile(workspace / "DQ8Recomp-source", workspace / "Extracted_Usa")
        self.assertEqual(general["input"], os.path.join("..", "Extracted_Usa", "SLUS_212.07"))
        self.assertEqual(general["output"], os.path.join("build", "generated", "SLUS_212.07"))
        self.assertEqual(call.args[0][1], os.path.join("build", "recompile-configs", "SLUS_212.07", "main.toml"))
        self.assertEqual(call.kwargs["cwd"], workspace / "DQ8Recomp-source")

    def test_name_beyond_u_ffff_reads_back(self):
        # JSON escapes this kanji as a surrogate pair, which TOML refuses.
        name = "\N{CJK UNIFIED IDEOGRAPH-20BB7}"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            general, _ = self.recompile(root / "source", root / name / "Extracted_Usa")
        self.assertEqual(general["input"], os.path.join("..", name, "Extracted_Usa", "SLUS_212.07"))


if __name__ == "__main__":
    unittest.main()
