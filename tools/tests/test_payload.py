import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "payload", Path(__file__).resolve().parents[1] / "launcher" / "payload.py")
payload = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(payload)


class PruneTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name)

    def touch(self, name):
        path = self.out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def test_tcl_and_tk_leave_unix_python(self):
        tcl = self.touch("tools/python/lib/tcl8.6/init.tcl")
        thread = self.touch("tools/python/lib/thread2.8.9/pkgIndex.tcl")
        threading = self.touch("tools/python/lib/python3.12/threading.py")
        payload.prune(self.out, "linux-x86_64")
        self.assertFalse(tcl.exists())
        self.assertFalse(thread.exists())
        self.assertTrue(threading.exists())

    def test_windows_python_keeps_its_standard_library(self):
        # Lib\ as Windows' case-blind disks show it to lib/: the standard library.
        threading = self.touch("tools/python/lib/threading.py")
        tcl = self.touch("tools/python/tcl/tcl8.6/init.tcl")
        payload.prune(self.out, "windows-x86_64")
        self.assertTrue(threading.exists())
        self.assertFalse(tcl.exists())


if __name__ == "__main__":
    unittest.main()
