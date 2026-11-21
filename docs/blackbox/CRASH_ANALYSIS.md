# Archived firmware crash analysis

Use **Symbolize Firmware Crash** in GitHub Actions to translate ESP32 crash
addresses with the exact ELF saved by **Deploy Firmware**. This analysis does not
compile firmware or update devices. The existing archive key must still match the
key used by the original deployment; creating a new key cannot recover an old
archive.

## Manual workflow

1. Open `.github/workflows/symbolize-firmware-crash.yml` in Actions and select
   **Run workflow** on the reviewed branch.
2. Supply the source deployment run ID, artifact ID, expected full ELF SHA-256,
   sketch ELF filename, and space-separated PC/backtrace addresses. The workflow
   defaults identify the retained generator v40 deployment below.
3. The workflow verifies the artifact belongs to the selected successful
   deployment in this repository, downloads the encrypted archive, and decrypts
   it with the repository secret `ELF_ARCHIVE_KEY`.
4. It checks the complete SHA-256 before calling `addr2line`. Read the job summary
   and symbol report. The key, decrypted ELF, and map are never report artifacts.

This workflow requires `contents: read` and `actions: read`. Pull-request checks
run synthetic tests only and do not use the archive key. To make the workflow
available in the normal Actions menu, it must be on the default branch. Repository
policy and GitHub may permit API dispatch on an existing non-default workflow
branch; do not merge an unreviewed workflow merely to obtain secrets.

The supported target is the original **32-bit little-endian Xtensa ESP32**.
The toolchain matches this archived deployment: Arduino ESP32 **3.3.11**, bundled
`esp-x32` **2601**. RISC-V ESP32 targets require a different symbolizer and file
validation and are intentionally rejected here.

## Generator v40 reference

- Source deployment run: `40188400587`
- Artifact ID: `12597655276`
- Artifact name: `firmware-elf-HAS1_generator-v40-40188400587`
- ELF: `HAS1_generator.ino.elf`
- Full ELF SHA-256:
  `17149407e0ec5a95355c8c051f7426ad163ab6a71225c65a1a3d20210266b69f`
- SG `btController` PC/backtrace:
  `0x4019a2c2 0x4019a3d0 0x4019a973 0x40097965`
- LG TASK_WDT captured `IDLE1` PC/backtrace:
  `0x400949bb 0x401d24ed 0x4009895f 0x40097965`
- LG `generator-net` debug exception PC:
  `0x40084965` (the recorded backtrace is marked corrupted)

The nine characters in `prior.core.elf:17149407e` select a candidate build. The
full SHA-256 from its deployment log is the required validation before decoding.
A fresh build from the same source is not automatically byte-identical: toolchain,
options, libraries, and generated configuration can change addresses.

## Local invocation

Obtain the encrypted archive from the original Actions artifact. Make the
existing key available as `ELF_ARCHIVE_KEY` through your secret-management method;
do not paste it into a shell command, source file, chat, or report.

```bash
python3 scripts/symbolize_firmware_crash.py \
  --archive /private/path/firmware-elf.tar.gz.enc \
  --sha256 17149407e0ec5a95355c8c051f7426ad163ab6a71225c65a1a3d20210266b69f \
  --elf-name HAS1_generator.ino.elf \
  --addr2line /path/to/esp-x32/2601/bin/xtensa-esp32-elf-addr2line \
  --addresses '0x4019a2c2 0x4019a3d0 0x4019a973 0x40097965 0x400949bb 0x401d24ed 0x4009895f 0x40084965' \
  --output /private/path/crash-symbols.md
```

Linux and macOS are supported. If macOS's bundled `openssl` does not support
PBKDF2, add `--openssl /opt/homebrew/opt/openssl@3/bin/openssl` (use your installed
path). The CLI requires a new report path, 1–32 hexadecimal addresses, and the
full 64-digit SHA. Missing keys, wrong keys, hash mismatches, unsafe tar members,
unsupported ELF architecture, and tool errors fail without producing a report.
Unknown addresses are reported as `??`/`?:0`; an unresolved symbol is not a
confirmed firmware fault location.

Decryption uses AES-256-CBC/PBKDF2 with 200,000 iterations, matching the existing
archive format. Plaintext exists only in a mode-0700 temporary directory and is
removed after success or failure. Only the selected regular-file ELF is unpacked;
the map is ignored. Archive/member sizes, command time, and symbol output are
bounded. The key is removed from the symbolizer's environment. Subprocess errors
are suppressed rather than copied to public logs. The report contains function
names and source locations, not source snippets, ELF contents, or disassembly.

## Interpretation

- Preserve address order and note whether the device marked the backtrace
  corrupted. A corrupt backtrace can yield incomplete or misleading callers.
- A task named in the dump is the captured task, not necessarily the task that
  failed to feed a watchdog. Use the full watchdog diagnostics to identify that
  task.
- Keep reset reasons, boot boundaries, and device uptime together. A Wi-Fi error
  after reboot cannot explain the preceding reboot by itself.
- Retained old dumps may be re-reported after power-on. Confirm fresh/reset flags
  and ELF identity before attributing a dump to the current incident.

## Tests

```bash
python3 -m unittest discover -s scripts -p 'test_symbolize_firmware_crash.py' -v
```

The tests encrypt fixtures with real OpenSSL, then exercise successful decoding,
wrong key/hash, unsupported ELF, unsafe/duplicate tar entries, size limits,
timeout, output limits, private temporary-file cleanup, and key isolation. They
use a synthetic symbolizer and no production key. Set
`OPENSSL_TEST_EXECUTABLE` if an alternate OpenSSL path is needed.
