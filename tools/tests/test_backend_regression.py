import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "gsdump/run_backend_regression.sh"


class BackendRegressionTests(unittest.TestCase):
    def test_numbered_dump_names_keep_separate_outputs(self):
        with tempfile.TemporaryDirectory(prefix="dq8 regression ") as directory:
            root = Path(directory)
            dumps = root / "dumps"
            dumps.mkdir()
            for name in ("scene.1.gs", "scene.2.gs.zst"):
                (dumps / name).touch()
            replay = root / "synthetic-replay"
            replay.write_text(
                "#!/usr/bin/env python3\nimport pathlib, sys\n"
                "if sys.argv[1] == 'replay':\n"
                "    output = sys.argv[sys.argv.index('--out') + 1]\n"
                "    pathlib.Path(output + '.raw32').touch()\n"
                "else:\n"
                "    print('psnr              : 99.00 dB')\n"
                "    print('mean abs error    : 0.00')\n")
            replay.chmod(0o755)
            output = root / "output"
            result = subprocess.run(["bash", str(SCRIPT), str(dumps), str(output)],
                                    env={**os.environ, "DQ8_GSREPLAY": str(replay)},
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ("scene.1", "scene.2"):
                for backend in ("sw", "hw"):
                    self.assertTrue((output / f"{name}_{backend}.raw32").exists())


if __name__ == "__main__":
    unittest.main()
