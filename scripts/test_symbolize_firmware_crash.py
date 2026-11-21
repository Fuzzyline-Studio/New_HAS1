"""python3 -m unittest discover -s scripts -p 'test_symbolize_firmware_crash.py'."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

import symbolize_firmware_crash as tool


OPENSSL = (os.environ.get("OPENSSL_TEST_EXECUTABLE")
           or ("/opt/homebrew/opt/openssl@3/bin/openssl"
               if Path("/opt/homebrew/opt/openssl@3/bin/openssl").exists() else shutil.which("openssl")))


class CrashSymbolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scratch = self.root / "private"
        self.scratch.mkdir()
        self.key = "test-only-not-a-production-key"
        self.env = mock.patch.dict(os.environ, {"ELF_ARCHIVE_KEY": self.key})
        self.env.start()
        self.addCleanup(self.env.stop)
        header = bytearray(52)
        header[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<H", header, 18, 94)
        self.elf = bytes(header) + b"test fixture: no actual program"
        self.sha = hashlib.sha256(self.elf).hexdigest()
        self.name = "HAS1_generator.ino.elf"
        self.archive = self.root / "input.enc"
        self.output = self.root / "report.md"
        self.symbolizer = self.root / "fake-addr2line"
        self.make_symbolizer("""import os, pathlib, sys
assert 'ELF_ARCHIVE_KEY' not in os.environ
assert sys.argv[1:3] == ['-pfiaC', '-e']
assert pathlib.Path(sys.argv[3]).read_bytes().startswith(b'\\x7fELF')
for address in sys.argv[4:]:
    print(address + ': test_function at fixture.cpp:17')
""")

    def make_symbolizer(self, body):
        self.symbolizer.write_text("#!/usr/bin/env python3\n" + body)
        self.symbolizer.chmod(0o700)

    def encrypt(self, members=None, payload=None):
        if members is None:
            members = [(self.name, self.elf, None), ("HAS1_generator.ino.map", b"private map", None)]
        raw = self.root / "fixture.tar.gz"
        if payload is None:
            with tarfile.open(raw, "w:gz") as bundle:
                for name, content, kind in members:
                    entry = tarfile.TarInfo(name)
                    entry.size = len(content)
                    if kind:
                        entry.type = kind
                        entry.linkname = "../../outside"
                    bundle.addfile(entry, io.BytesIO(content))
        else:
            raw.write_bytes(payload)
        subprocess.run([OPENSSL, "enc", "-aes-256-cbc", "-pbkdf2", "-iter", "200000", "-salt",
                        "-pass", "env:ELF_ARCHIVE_KEY", "-in", str(raw), "-out", str(self.archive)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        raw.unlink()

    def analyze(self, **overrides):
        args = dict(archive=self.archive, sha256=self.sha, elf_name=self.name,
                    addr2line=str(self.symbolizer), addresses="0x4019a2c2 0x40084965",
                    output=self.output, openssl=OPENSSL, temp_parent=self.scratch)
        args.update(overrides)
        try:
            return tool.analyze(**args)
        finally:
            self.assertEqual(list(self.scratch.iterdir()), [], "private plaintext was not cleaned up")

    def assert_failure(self, pattern, **overrides):
        with self.assertRaisesRegex(tool.AnalysisError, pattern):
            self.analyze(**overrides)
        self.assertFalse(self.output.exists())

    def test_roundtrip_hash_check_symbols_only_and_no_key_in_child(self):
        self.encrypt()
        self.analyze()
        report = self.output.read_text()
        self.assertIn(self.sha, report)
        self.assertIn("test_function at fixture.cpp:17", report)
        self.assertNotIn(self.key, report)
        self.assertNotIn("private map", report)
        self.assertNotIn(self.elf[52:].decode(), report)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()),
                         ["fake-addr2line", "input.enc", "private", "report.md"])

    def test_wrong_key_does_not_leave_plaintext(self):
        self.encrypt()
        with mock.patch.dict(os.environ, {"ELF_ARCHIVE_KEY": "wrong-test-key"}):
            self.assert_failure("command failed|archive is invalid")

    def test_hash_mismatch_never_runs_symbolizer(self):
        self.encrypt()
        self.make_symbolizer("raise AssertionError('must not run')\n")
        self.assert_failure("SHA-256 mismatch", sha256="0" * 64)

    def test_key_is_required(self):
        self.encrypt()
        with mock.patch.dict(os.environ, {"ELF_ARCHIVE_KEY": ""}):
            self.assert_failure("missing")

    def test_short_sha_and_invalid_addresses_are_rejected(self):
        for addresses in ("", "0x100000000", "-e /secret", "0x12;echo bad", "12", "0x1 " * 33):
            with self.subTest(addresses=addresses):
                self.assert_failure("addresses|Addresses", addresses=addresses)
        self.assert_failure("64-digit", sha256=self.sha[:9])

    def test_missing_wrong_or_unsafe_elf_names(self):
        self.encrypt()
        self.assert_failure("basename", elf_name="../HAS1_generator.ino.elf")
        self.assert_failure("unexpected ELF", elf_name="different.ino.elf")
        self.encrypt(members=[("HAS1_generator.ino.map", b"map", None)])
        self.assert_failure("absent")

    def test_duplicate_link_traversal_and_extra_members_rejected(self):
        cases = [
            [(self.name, self.elf, None), (self.name, self.elf, None)],
            [(self.name, self.elf, None), ("HAS1_generator.ino.map", b"", tarfile.SYMTYPE)],
            [(self.name, self.elf, None), ("HAS1_generator.ino.map", b"", tarfile.LNKTYPE)],
            [("../" + self.name, self.elf, None)],
            [("/" + self.name, self.elf, None)],
            [(self.name, self.elf, None), ("private.txt", b"data", None)],
        ]
        for members in cases:
            with self.subTest(names=[m[0] for m in members]):
                self.encrypt(members=members)
                self.assert_failure("unsafe member")

    def test_non_xtensa_and_non_elf_rejected_even_with_matching_hash(self):
        for data in (b"not an ELF", self.elf[:18] + b"\x03\x00" + self.elf[20:]):
            with self.subTest(data=data[:20]):
                self.encrypt(members=[(self.name, data, None)])
                self.assert_failure("Xtensa ELF", sha256=hashlib.sha256(data).hexdigest())

    def test_archive_and_member_size_limits(self):
        self.encrypt()
        with mock.patch.object(tool, "MAX_ARCHIVE_BYTES", 1):
            self.assert_failure("too large")
        with mock.patch.object(tool, "MAX_MEMBER_BYTES", 10):
            self.assert_failure("unpacked size")
        with mock.patch.object(tool, "MAX_TOTAL_BYTES", 10):
            self.assert_failure("unpacked size")

    def test_corrupt_archive_suppresses_internal_error(self):
        self.encrypt(payload=b"PRIVATE ERROR CONTENT NOT A GZIP")
        self.assert_failure("archive is invalid")

    def test_failed_tool_output_is_never_reported(self):
        self.encrypt()
        self.make_symbolizer("import sys\nprint('PRIVATE ELF DATA')\nprint('PRIVATE ERROR',file=sys.stderr)\nsys.exit(1)\n")
        self.assert_failure("command failed")

    def test_tool_timeout_cleans_up(self):
        self.encrypt()
        self.make_symbolizer("import time\ntime.sleep(10)\n")
        with mock.patch.object(tool, "COMMAND_TIMEOUT", 1):
            self.assert_failure("time limit")

    def test_tool_output_is_bounded(self):
        self.encrypt()
        self.make_symbolizer("import sys\nsys.stdout.write('x' * (1024 * 1024))\n")
        self.assert_failure("command failed|report limit")

    def test_report_escapes_control_markup_and_preserves_unknown_symbols(self):
        self.encrypt()
        self.make_symbolizer("print('0x40000000: ?? at ??:0')\nprint('<script>bad</script>\\x1b')\nprint('::error::text')\n")
        self.analyze()
        report = self.output.read_text()
        self.assertIn("?? at ??:0", report)
        self.assertIn("    &lt;script&gt;", report)
        self.assertNotIn("\x1b", report)
        self.assertIn("\n    ::error::text", report)

    def test_existing_report_is_not_overwritten(self):
        self.encrypt()
        self.output.write_text("previous report")
        with self.assertRaisesRegex(tool.AnalysisError, "already exists"):
            self.analyze()
        self.assertEqual(self.output.read_text(), "previous report")


if __name__ == "__main__":
    unittest.main()
