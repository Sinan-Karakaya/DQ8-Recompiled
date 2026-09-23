"""Synthetic tests; no game data or key is used."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import mvi_decrypt


class MviTests(unittest.TestCase):
    def test_key_is_required(self):
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as caught:
            mvi_decrypt.main(["--in", "unused", "--out", "unused"])
        self.assertEqual(caught.exception.code, 2)
        self.assertIn("--key", error.getvalue())

    def test_empty_key_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            key = Path(directory) / "key.bin"
            key.touch()
            with self.assertRaises(ValueError):
                mvi_decrypt.load_key(key)

    def test_periodic_xor_and_trailer_removal(self):
        key = bytes(range(256)) * 8
        plaintext = b"\x00\x00\x01\xba" + b"\x55" * (mvi_decrypt.BLOCK - 4)
        encrypted = bytes(value ^ key[index % len(key)]
                          for index, value in enumerate(plaintext))
        self.assertEqual(mvi_decrypt.decrypt_bytes(encrypted + b"tail", key), plaintext)
        with self.assertRaises(ValueError):
            mvi_decrypt.decrypt_bytes(encrypted, key)


if __name__ == "__main__":
    unittest.main()
