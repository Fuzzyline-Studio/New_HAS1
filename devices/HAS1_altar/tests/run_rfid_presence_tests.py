#!/usr/bin/env python3
"""Exercise production RFID presence/gain logic with host PN532 and callback doubles.

Only RfidLoop's local buffer is instrumented: bytes beyond the actual four-byte
page (if any) get different poison on each poll. This deterministically exposes
the original 32-byte comparison without depending on undefined stack contents.
CardChecking is a callback double; HTTP, rewards and physical RF are not tested.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"(?:static )?(?:void|bool)\s+" + name + r"\s*\([^)]*\)\s*\{", source)
    if not match:
        raise RuntimeError(f"Missing firmware function: {name}")
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


header = (ROOT / "HAS1_altar.h").read_text(encoding="utf-8")
sensor = (ROOT / "sensor.ino").read_text(encoding="utf-8")
constants = "\n".join(line for line in header.splitlines() if
                      line.startswith(("enum GainMode", "static constexpr size_t RFID_PAGE_DATA_SIZE",
                                       "#define TAG_REMOVE_TIME_MS", "#define RFID_ACTIVATION_RETRIES",
                                       "#define RFID_DETECT_TIMEOUT_MS")))
state = "\n".join(line for line in sensor.splitlines() if
                  re.match(r"static (?:GainMode|bool|uint8_t|unsigned long) pn532_", line)
                  or re.match(r"static RfidRecoveryPolicy rfid_recovery;", line)
                  or re.match(r"static const char \*rfid_fault_reason = ", line)
                  or re.match(r"static bool (?:tag_active|chip_return_processed|tag_data_fetched) =", line))
loop = function(sensor, "RfidLoop")
loop, count = re.subn(r"(uint8_t data\[[^]]+\](?:\s*=\s*\{0\})?;)",
                      r"\1\n  for (size_t i = 4; i < sizeof(data); ++i) data[i] = ++poison;", loop)
assert count == 1, "Expected exactly one local card buffer"
source = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include "recovery_policy.h"
using byte = uint8_t;
#define BREADCRUMB(x) ((void)0)
#define PN532_MIFARE_ISO14443A 0
unsigned long now_ms = 0;
unsigned long millis() { return now_ms; }
uint8_t poison = 0;
struct SerialDouble {
  void println(const char *) {}
  template <typename... A> void printf(const char *, A...) {}
} Serial;
char g_crash_msg[200] = "";
bool g_crash_pending = false;
// Server document and HTTP doubles used only by RfidMaintain's boot-failure restore path.
struct String {
  std::string s;
  String(const char *v) : s(v ? v : "") {}
  bool operator==(const char *o) const { return s == o; }
};
const char *my_device_state = "activate";
struct Doc { const char *operator[](const char *key) {
  return std::string(key) == "device_state" ? my_device_state : "AA"; } } my;
int state_sends = 0;
struct Wifi { void Send(String, const char *, const char *) { ++state_sends; } } has2wifi;
bool rfid_tag = false;
int rfid_timer_id = 0;
void RfidTagTimerFunc() { rfid_tag = false; }
struct Timer { int setTimeout(int, void (*)()) { return 1; } } rfid_timer;
struct Read { bool ack; bool detect; bool success; std::string data; };
std::deque<Read> reads;
int gains_applied = 0;
bool reader_alive = true;  // false: every ACK-required command (gain, init) fails like a dead PN532
int gain_fail_at = 0;      // >0: only the N-th gain command (counted in gains_applied) fails
struct Reader {
  // Only ApplyGain still goes through the raw command path; it is the liveness probe.
  bool sendCommandCheckAck(uint8_t *, size_t, int = 0) {
    ++gains_applied;
    if (gain_fail_at && gains_applied == gain_fail_at) return false;
    return reader_alive;
  }
  // readPassiveTargetID consumes the InListPassiveTarget response: a missing ACK or
  // no target both come back as false and end the attempt.
  bool readPassiveTargetID(int, uint8_t *, uint8_t *, int) {
    assert(!reads.empty());
    if (!reads.front().ack || !reads.front().detect) { reads.pop_front(); return false; }
    return true;
  }
  bool begin() { return true; }
  uint32_t getFirmwareVersion() { return reader_alive ? 0x32010607u : 0u; }
  bool SAMConfig() { return reader_alive; }
  bool setPassiveActivationRetries(uint8_t) { return reader_alive; }
  bool ntag2xx_ReadPage(int page, uint8_t *out) {
    assert(page == 7 && !reads.empty());
    Read result = reads.front(); reads.pop_front();
    // Failed transactions can modify the buffer too: caller must ignore it.
    memcpy(out, result.data.data(), 4);
    return result.success;
  }
} nfc;
void CardChecking(uint8_t *);
// Blackbox hook double: counts the reader/tag records RfidLoop and recovery request.
int tag_left_records = 0, reinit_records = 0, recovered_records = 0;
void BlackBoxAltar(const char *label, int32_t, int32_t, int32_t, int32_t) {
  const std::string name = label;
  assert(name.size() <= 27);
  if (name == "altar.tag.left") ++tag_left_records;
  else if (name == "altar.rfid.reinit") ++reinit_records;
  else if (name == "altar.rfid.recovered") ++recovered_records;
  else assert(!"unexpected blackbox record");
}
// PRODUCTION
int card_checks = 0, fetches = 0, returns = 0;
std::string checked_code;
void CardChecking(uint8_t *data) {
  ++card_checks;
  checked_code.assign(reinterpret_cast<char *>(data), 4);
  tag_active = true;
  if (!tag_data_fetched) { tag_data_fetched = true; ++fetches; }
  if (!chip_return_processed) { chip_return_processed = true; ++returns; }
}
Read card(const char *code = "G1P2") { return {true, true, true, code}; }
Read miss() { return {true, true, false, "XXXX"}; }
void poll(unsigned long at, std::initializer_list<Read> attempts) {
  assert(reads.empty());
  now_ms = at; reads = attempts;
  RfidTagTimerFunc(); RfidLoop();
  assert(reads.empty());
}
void reset() {
  reads.clear(); pn532_gain = GAIN_NEAR; pn532_tag_locked = false;
  pn532_last_seen_ms = 0; tag_active = chip_return_processed = tag_data_fetched = false;
  card_checks = fetches = returns = gains_applied = 0;
  reader_alive = true; gain_fail_at = 0; g_crash_pending = false; g_crash_msg[0] = '\0';
  my_device_state = "activate"; state_sends = 0;
  rfid_recovery = RfidRecoveryPolicy(); rfid_recovery.recordSuccess();
}
bool crash_note_is(const char *prefix) {
  return g_crash_pending && std::string(g_crash_msg).rfind(prefix, 0) == 0;
}
int main() {
  // Same code over many polls keeps all session gates set despite changing tail poison.
  reset();
  for (int i = 0; i < 20; ++i) poll(i * 1000, {card()});
  assert(pn532_tag_locked && tag_active && chip_return_processed && tag_data_fetched);
  assert(card_checks == 20 && returns == 1 && fetches == 1 && gains_applied == 0);
  // Poll throttle prevents an extra card read/callback.
  RfidLoop(); assert(card_checks == 20);

  // Failure inside grace period preserves gates; successful retry extends last-seen.
  poll(20000, {miss(), miss()});
  assert(pn532_tag_locked && chip_return_processed && tag_data_fetched && card_checks == 20);
  poll(21000, {miss(), card()});
  assert(pn532_gain == GAIN_FAR && pn532_last_seen_ms == 21000);
  assert(returns == 1 && fetches == 1);
  poll(23499, {miss(), miss()});
  assert(pn532_tag_locked && tag_active && chip_return_processed && tag_data_fetched);
  poll(23500, {miss(), miss()});
  assert(!pn532_tag_locked && !tag_active && !chip_return_processed && !tag_data_fetched);
  assert(pn532_gain == GAIN_NEAR);
  poll(24500, {card()}); assert(returns == 2 && fetches == 2);

  // A genuinely different four-byte code must not sustain the old session.
  reset(); poll(0, {card()});
  poll(1000, {card("G1P3"), card("G1P3")});
  assert(pn532_tag_locked && card_checks == 1 && checked_code == "G1P2");
  poll(2500, {card("G1P3"), card("G1P3")});
  assert(!pn532_tag_locked && !chip_return_processed && !tag_data_fetched);
  poll(3500, {card("G1P3")});
  assert(checked_code == "G1P3" && fetches == 2 && returns == 2);

  // Search retries the other gain; command/detection failures never process stale data.
  reset(); poll(0, {{false, true, true, "G1P2"}, card()});
  assert(pn532_tag_locked && pn532_gain == GAIN_FAR && card_checks == 1);
  poll(1000, {{true, false, true, "G1P2"}, {false, true, true, "G1P2"}});
  assert(pn532_tag_locked && card_checks == 1 && chip_return_processed);
  poll(2500, {miss(), miss()}); assert(!pn532_tag_locked && card_checks == 1);
  reset(); poll(0, {miss(), miss()});
  assert(!pn532_tag_locked && card_checks == 0 && !tag_data_fetched);

  // Reader death while a tag is held: the failed gain command schedules recovery and
  // immediately drops tag_active (fail-closed: the crank must not open the solenoid on a
  // reader we cannot trust). The tag lock and the per-session payout/fetch gates survive the
  // fault, so a reader hiccup never pays the chip return twice or re-fires the blocking fetch.
  reset(); poll(0, {card()});
  assert(pn532_tag_locked && tag_active && chip_return_processed && tag_data_fetched);
  reader_alive = false;
  poll(1000, {miss()});
  assert(!rfid_recovery.ready() && pn532_tag_locked);
  assert(!tag_active && chip_return_processed && tag_data_fetched);
  assert(crash_note_is("pn532=reinit_requested gain_switch_failed"));
  poll(2000, {});  // not ready: the chip is left alone, nothing is consumed, tag stays absent
  assert(!tag_active && chip_return_processed && card_checks == 1);

  // Backoff: a dead reader fails init immediately, then waits 1s, 2s, ... before retrying.
  now_ms = 2000; RfidMaintain();
  assert(!rfid_recovery.ready() && rfid_recovery.failureCount() == 1);
  now_ms = 2999; RfidMaintain(); assert(rfid_recovery.failureCount() == 1);
  now_ms = 3000; RfidMaintain(); assert(rfid_recovery.failureCount() == 2);
  now_ms = 4999; RfidMaintain(); assert(rfid_recovery.failureCount() == 2);

  // Reader returns: init re-applies NEAR gain, reports the recovery with the original
  // reason and attempt count, and the held tag continues its session: tag_active comes
  // back, but nothing is fetched or paid out again.
  reader_alive = true; g_crash_pending = false;
  now_ms = 5000; RfidMaintain();
  assert(rfid_recovery.ready() && rfid_recovery.failureCount() == 0 && pn532_gain == GAIN_NEAR);
  assert(crash_note_is("pn532=recovered gain_switch_failed") &&
         std::string(g_crash_msg).find("fails=2") != std::string::npos);
  assert(state_sends == 0);  // device_state untouched: it was never set to PN532 by us
  poll(6000, {card()});
  assert(pn532_tag_locked && tag_active && card_checks == 2 && fetches == 1 && returns == 1);

  // Same fault, but the tag is gone when the reader returns: the grace period (measured from
  // the last confirmed sighting) has long expired, so removal is confirmed on the first poll
  // and the gates reset normally for the next tagger.
  reset(); poll(0, {card()}); reader_alive = false; poll(1000, {miss()});
  reader_alive = true; now_ms = 5000; RfidMaintain(); assert(rfid_recovery.ready());
  poll(6000, {miss(), miss()});
  assert(!pn532_tag_locked && !tag_active && !chip_return_processed && !tag_data_fetched);
  poll(7000, {card()}); assert(returns == 2 && fetches == 2);

  // ... and a different card after recovery is not mistaken for the old session either.
  reset(); poll(0, {card()}); reader_alive = false; poll(1000, {miss()});
  reader_alive = true; now_ms = 5000; RfidMaintain();
  poll(6000, {card("G1P3"), card("G1P3")});
  assert(!pn532_tag_locked && !chip_return_processed && card_checks == 1);
  poll(7000, {card("G1P3")}); assert(checked_code == "G1P3" && fetches == 2 && returns == 2);

  // A pending real crash report is never overwritten by a reader event.
  reset(); strcpy(g_crash_msg, "reason=PANIC uptime=5ms"); g_crash_pending = true;
  reader_alive = false; poll(0, {miss()});
  assert(!rfid_recovery.ready() && std::string(g_crash_msg) == "reason=PANIC uptime=5ms");

  // Boot-time failure path: we had reported device_state=PN532, so a later recovery
  // restores "setting" (and only then - see the state_sends == 0 assertion above).
  reset(); rfid_recovery = RfidRecoveryPolicy(); rfid_recovery.recordFailure(0);
  my_device_state = "PN532"; now_ms = 1000; RfidMaintain();
  assert(rfid_recovery.ready() && state_sends == 1);

  // Search mode with a dead reader: no tag plus a failed gain switch is a reader fault,
  // not "no tag", so recovery is scheduled from the very first empty poll.
  reset(); reader_alive = false; poll(0, {miss()});
  assert(!rfid_recovery.ready() && !pn532_tag_locked && card_checks == 0);
  assert(crash_note_is("pn532=reinit_requested gain_switch_failed"));

  // Tag removal with a dead reader: the gain reset after the grace period is also a probe.
  reset(); poll(0, {card()}); reader_alive = false;
  poll(1000, {miss()});  // locked miss -> gain switch fails -> immediate recovery
  assert(!rfid_recovery.ready() && !tag_active);

  // Removal confirmed at grace expiry, then the gain reset itself fails: the session gates
  // must already be down, so the next player after recovery gets a fresh fetch and payout
  // instead of the previous player's cached tag data.
  reset(); poll(0, {card()});
  poll(1000, {miss(), miss()});           // one miss inside the grace period
  gains_applied = 0; gain_fail_at = 3;    // this tick: switch ok, restore ok, reset probe dies
  poll(2500, {miss(), miss()});           // grace expired -> removal confirmed -> reset fails
  gain_fail_at = 0;
  assert(!pn532_tag_locked && !rfid_recovery.ready());
  assert(!tag_active && !chip_return_processed && !tag_data_fetched);
  now_ms = 10000; RfidMaintain(); assert(rfid_recovery.ready());
  poll(11000, {card("G1P3")});
  assert(checked_code == "G1P3" && fetches == 2 && returns == 2);
  // Every reader fault that scheduled a re-init and every recovery left a record.
  assert(reinit_records > 0 && recovered_records > 0 && tag_left_records > 0);
  std::cout << "PASS: altar production RFID presence, retries, debounce, gates, card replacement and reader recovery\n";
}
'''.replace('// PRODUCTION', constants + '\n' + state + '\n' +
            '\n'.join(function(sensor, name) for name in
                      ('ApplyGain', 'DetectAndRead', 'RfidInit', 'RfidReportEvent',
                       'RequestRfidReinit', 'RfidMaintain')) + '\n' + loop)
with tempfile.TemporaryDirectory(prefix='altar-rfid-') as tmp:
    src, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    src.write_text(source, encoding="utf-8")
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
