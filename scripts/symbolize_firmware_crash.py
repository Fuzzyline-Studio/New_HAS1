#!/usr/bin/env python3
"""Decrypt an archived, hash-pinned ESP32 ELF and produce symbols only."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import resource
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile


MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_MEMBER_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_SYMBOL_BYTES = 256 * 1024
COMMAND_TIMEOUT = 60


class AnalysisError(Exception):
    """A safe, operator-facing error that never contains subprocess output."""


def parse_addresses(value: str) -> list[str]:
    tokens = value.split()
    if not 1 <= len(tokens) <= 32:
        raise AnalysisError("Provide between 1 and 32 hexadecimal addresses.")
    if any(not re.fullmatch(r"0x[0-9a-fA-F]{1,8}", token) for token in tokens):
        raise AnalysisError("Addresses must be 32-bit hexadecimal values starting with 0x.")
    return [f"0x{int(token, 16):08x}" for token in tokens]


def validate_inputs(sha256: str, elf_name: str, addresses: str) -> list[str]:
    if not re.fullmatch(r"[0-9a-fA-F]{64}", sha256):
        raise AnalysisError("The complete 64-digit ELF SHA-256 is required.")
    if not re.fullmatch(r"[A-Za-z0-9_-]+\.ino\.elf", elf_name):
        raise AnalysisError("ELF name must be a sketch basename ending in .ino.elf.")
    return parse_addresses(addresses)


def run_private(command: list[str], *, env: dict[str, str], output: Path | None = None,
                max_bytes: int = MAX_SYMBOL_BYTES) -> None:
    """Never expose stderr: decrypted files or tools may include private data."""
    try:
        if output is None:
            result = subprocess.run(command, env=env, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, timeout=COMMAND_TIMEOUT, check=False)
        else:
            with output.open("xb") as stream:
                os.chmod(output, 0o600)
                result = subprocess.run(command, env=env, stdout=stream,
                                        stderr=subprocess.DEVNULL, timeout=COMMAND_TIMEOUT, check=False,
                                        preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_FSIZE,
                                                                             (max_bytes, max_bytes)))
        if result.returncode:
            raise AnalysisError("A cryptography or symbolization command failed; private output was suppressed.")
    except subprocess.TimeoutExpired:
        raise AnalysisError("A cryptography or symbolization command exceeded its time limit.") from None
    except OSError:
        raise AnalysisError("A required command or private temporary file could not be accessed.") from None


def extract_verified_elf(archive: Path, directory: Path, elf_name: str, sha256: str) -> Path:
    """Stream only a regular, named ELF; never use extract()/extractall()."""
    elf_path = directory / "verified.elf"
    seen: set[str] = set()
    total = 0
    try:
        with tarfile.open(archive, mode="r|gz") as bundle:
            for member in bundle:
                if (len(seen) >= 8 or member.name in seen or not member.isfile()
                        or member.issparse()
                        or not re.fullmatch(r"[A-Za-z0-9_-]+\.ino\.(elf|map)", member.name)):
                    raise AnalysisError("Archive contains an unsupported, duplicate, or unsafe member.")
                seen.add(member.name)
                total += member.size
                if member.size < 0 or member.size > MAX_MEMBER_BYTES or total > MAX_TOTAL_BYTES:
                    raise AnalysisError("Archive exceeds the permitted unpacked size.")
                if member.name.endswith(".elf") and member.name != elf_name:
                    raise AnalysisError("Archive contains an unexpected ELF.")
                if member.name != elf_name:
                    continue
                source = bundle.extractfile(member)
                if source is None:
                    raise AnalysisError("The archived ELF could not be read.")
                digest = hashlib.sha256()
                copied = 0
                with source, elf_path.open("xb") as destination:
                    os.chmod(elf_path, 0o600)
                    while chunk := source.read(1024 * 1024):
                        copied += len(chunk)
                        if copied > member.size:
                            raise AnalysisError("Archived ELF size is inconsistent.")
                        digest.update(chunk)
                        destination.write(chunk)
                if copied != member.size or digest.hexdigest() != sha256.lower():
                    raise AnalysisError("ELF SHA-256 mismatch; symbolization was not attempted.")
    except (tarfile.TarError, EOFError, OSError):
        raise AnalysisError("The decrypted archive is invalid or unreadable.") from None
    if elf_name not in seen:
        raise AnalysisError("The requested ELF is absent from the archive.")
    with elf_path.open("rb") as stream:
        header = stream.read(52)
    if (len(header) < 52 or header[:7] != b"\x7fELF\x01\x01\x01"
            or struct.unpack_from("<H", header, 18)[0] != 94):
        raise AnalysisError("Verified file is not a 32-bit little-endian Xtensa ELF.")
    return elf_path


def render_report(elf_name: str, sha256: str, addresses: list[str], symbols: str) -> str:
    # Escape Markdown/HTML and neutralize workflow-command/control characters.
    # Output consists only of addr2line's function/source-location text, never
    # source snippets, strings, disassembly, or the private ELF/map.
    lines = []
    for line in symbols.splitlines():
        clean = "".join(c for c in line if c == "\t" or ord(c) >= 32)
        clean = clean.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        lines.append("    " + clean)
    return ("# Firmware crash symbols\n\n"
            f"ELF: `{elf_name}`\n\nSHA-256: `{sha256.lower()}`\n\n"
            f"Addresses: {' '.join(f'`{address}`' for address in addresses)}\n\n"
            "Exact archived ELF hash verified before symbolization. `??` or `?:0` means "
            "the address could not be resolved; it does not prove a firmware fault location. "
            "A captured task name is not necessarily the task that caused a watchdog reset.\n\n"
            + "\n".join(lines) + "\n")


def analyze(*, archive: Path, sha256: str, elf_name: str, addr2line: str,
            addresses: str, output: Path, openssl: str = "openssl",
            temp_parent: Path | None = None) -> None:
    parsed = validate_inputs(sha256, elf_name, addresses)
    key = os.environ.get("ELF_ARCHIVE_KEY", "")
    if not key:
        raise AnalysisError("ELF_ARCHIVE_KEY is missing; use the key that encrypted this archive.")
    if not archive.is_file() or not 0 < archive.stat().st_size <= MAX_ARCHIVE_BYTES:
        raise AnalysisError("Encrypted archive is missing, empty, or too large.")
    openssl = shutil.which(openssl)
    symbolizer = shutil.which(addr2line)
    if not openssl or not symbolizer:
        raise AnalysisError("OpenSSL and an Xtensa addr2line executable are required.")
    # Never leave an old report behind when this invocation fails.
    if output.exists():
        raise AnalysisError("Report already exists; choose a new output path.")
    with tempfile.TemporaryDirectory(prefix="firmware-symbols-", dir=temp_parent) as scratch:
        private = Path(scratch)
        os.chmod(private, 0o700)
        tar_path = private / "archive.tar.gz"
        env = dict(os.environ)
        run_private([openssl, "enc", "-d", "-aes-256-cbc", "-pbkdf2", "-iter", "200000",
                     "-pass", "env:ELF_ARCHIVE_KEY", "-in", str(archive)], env=env, output=tar_path,
                    max_bytes=MAX_ARCHIVE_BYTES)
        elf_path = extract_verified_elf(tar_path, private, elf_name, sha256)
        env.pop("ELF_ARCHIVE_KEY", None)
        raw = private / "symbols.txt"
        run_private([symbolizer, "-pfiaC", "-e", str(elf_path), *parsed], env=env, output=raw)
        if raw.stat().st_size > MAX_SYMBOL_BYTES:
            raise AnalysisError("Symbolization output exceeds the report limit.")
        symbols = raw.read_text(encoding="utf-8", errors="replace").strip()
        if not symbols:
            raise AnalysisError("The symbolizer returned no result.")
        report = render_report(elf_name, sha256, parsed, symbols)
    # Only the non-sensitive symbol report leaves the private directory.
    try:
        with output.open("x", encoding="utf-8") as stream:
            stream.write(report)
    except OSError:
        raise AnalysisError("The symbol report could not be created.") from None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--elf-name", required=True)
    parser.add_argument("--addr2line", required=True)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--addresses", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        analyze(**vars(args))
    except AnalysisError as error:
        print(f"Analysis failed: {error}", file=sys.stderr)
        return 1
    print("Verified ELF symbol report created.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
