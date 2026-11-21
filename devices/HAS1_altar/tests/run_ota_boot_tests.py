#!/usr/bin/env python3
"""Drive the production OTA-only boot policy through reset and server-state sequences."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCE = r'''
#include <cassert>
#include <cstring>
#include <iostream>
#include "ota_boot_policy.h"

// Models game_state.ino: one boot, then what the device does with the server state.
// Returns what ended the boot.
enum End { Running, RestartForOta, RestartToNormal, Crashed };
struct Device {
  OtaBootState rtc;
  bool lean = false;
  int lean_boots = 0, normal_boots = 0, checks = 0;
  void boot(bool power_on) {
    lean = OtaBootPolicy::begin(rtc, power_on);
    ++(lean ? lean_boots : normal_boots);
  }
  // crash: the OTA-only boot dies before or during the check (panic/watchdog reset).
  End serverState(bool github, bool crash = false) {
    if (github) {
      if (lean) {
        if (crash) return Crashed;
        ++checks;                    // ota.check() returned without applying an update
        return RestartToNormal;
      }
      if (!OtaBootPolicy::mayRequest(rtc)) return Running;   // skipped, no reboot
      OtaBootPolicy::request(rtc);
      return RestartForOta;
    }
    OtaBootPolicy::leftGithub(rtc);
    return lean ? RestartToNormal : Running;
  }
};

int main() {
  // Power-on with garbage in RTC memory, and the first boot after an OTA from firmware
  // that never wrote this state (warm reset, arbitrary bytes): always a normal boot.
  for (int power_on = 0; power_on < 2; ++power_on) {
    for (uint32_t fill : {0u, 0xffffffffu, 0xdeadbeefu, OtaBootPolicy::kMagic}) {
      Device d;
      std::memset(&d.rtc, 0, sizeof(d.rtc));
      d.rtc.magic = fill; d.rtc.request = fill; d.rtc.attempts = fill;
      d.boot(power_on);
      assert(!d.lean && d.rtc.request == 0 && d.rtc.attempts == 0);
    }
  }

  // Normal case: github -> OTA-only boot -> check finds nothing, server reports setting.
  {
    Device d{}; d.boot(true);
    assert(d.serverState(true) == RestartForOta);
    d.boot(false); assert(d.lean);
    assert(d.serverState(true) == RestartToNormal && d.checks == 1);
    d.boot(false); assert(!d.lean);
    assert(d.serverState(false) == Running && d.rtc.attempts == 0);
  }

  // The OTA-only boot crashes every time while the server stays on github:
  // bounded number of OTA-only boots, then the device stays up in a normal boot.
  {
    Device d{}; d.boot(true);
    End end = d.serverState(true);
    int resets = 0;
    while (end != Running) {
      assert(++resets < 20);
      d.boot(false);
      end = d.serverState(true, /*crash=*/true);
    }
    assert(!d.lean && d.lean_boots == int(OtaBootPolicy::kMaxAttempts) && d.checks == 0);
    // Still skipped on later polls and after another warm reset.
    assert(d.serverState(true) == Running);
    d.boot(false); assert(!d.lean && d.serverState(true) == Running);
    // Leaving github re-arms a fresh instruction.
    assert(d.serverState(false) == Running);
    assert(d.serverState(true) == RestartForOta);
  }

  // The check fails or SecureOTA restarts after a partition update while the server
  // still says github: the next normal boot tries again, up to the cap.
  {
    Device d{}; d.boot(true);
    assert(d.serverState(true) == RestartForOta);
    for (uint32_t attempt = 1; attempt <= OtaBootPolicy::kMaxAttempts; ++attempt) {
      d.boot(false); assert(d.lean);
      assert(d.serverState(true) == RestartToNormal);
      d.boot(false); assert(!d.lean);
      End next = d.serverState(true);
      assert(next == (attempt < OtaBootPolicy::kMaxAttempts ? RestartForOta : Running));
    }
    assert(d.checks == int(OtaBootPolicy::kMaxAttempts));
  }

  // The server no longer says github when the OTA-only boot gets its first state:
  // leave that boot at once instead of running a game without BLE and the logger.
  {
    Device d{}; d.boot(true);
    assert(d.serverState(true) == RestartForOta);
    d.boot(false); assert(d.lean);
    assert(d.serverState(false) == RestartToNormal && d.checks == 0);
    d.boot(false); assert(!d.lean && d.serverState(false) == Running);
  }

  // A power cut during the OTA-only sequence forgets it: normal boot, fresh count.
  {
    Device d{}; d.boot(true);
    assert(d.serverState(true) == RestartForOta);
    d.boot(true);
    assert(!d.lean && d.rtc.attempts == 0 && d.serverState(true) == RestartForOta);
  }
  std::cout << "PASS: altar OTA-only boot policy (garbage RTC, crash loop cap, retry, early exit, power cut)\n";
}
'''

with tempfile.TemporaryDirectory(prefix="altar-ota-boot-") as temp:
    source, executable = Path(temp) / "test.cpp", Path(temp) / "test"
    source.write_text(SOURCE, encoding="utf-8")
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(ROOT), str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
