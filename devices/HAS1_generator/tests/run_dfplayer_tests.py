#!/usr/bin/env python3
"""Compile the actual vendored DFPlayer driver against a scripted public Stream.

No private-state access or replacement DFPlayer implementation. Simulated time
and UART input reproduce missing ACKs, retained events, rollover and noise.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
LIBRARY = Path(os.environ.get('DFPLAYER_LIBRARY', ROOT / 'libraries/DFRobotDFPlayerMini'))
APP = Path(os.environ.get('DFPLAYER_APP', ROOT / 'devices/HAS1_generator/dfplayer.ino'))
ARDUINO = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
unsigned long millis();
void delay(unsigned long ms);
class Stream {
 public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual size_t write(const uint8_t *, size_t) = 0;
};
'''
HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <stdexcept>
#include <vector>
#include "DFRobotDFPlayerMini.h"
static uint32_t nowMs;
static unsigned elapsed;
static bool tickMillis=false;
unsigned long millis() { if(tickMillis) { ++nowMs; ++elapsed; } return nowMs; }
void delay(unsigned long ms) {
  const unsigned step = ms ? ms : 1;
  nowMs += step; elapsed += step;
  if (elapsed > 5000) throw std::runtime_error("DFPlayer exceeded 5s total test budget");
}
static void check(bool ok, const char *why) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
class ScriptedStream : public Stream {
 public:
  std::deque<uint8_t> incoming;
  std::vector<std::vector<uint8_t>> sent;
  bool noise = false;
  unsigned reads = 0;
  int available() override { return noise || !incoming.empty(); }
  int read() override {
    ++reads;
    if (incoming.empty()) return noise ? 0x00 : -1;
    const uint8_t value = incoming.front(); incoming.pop_front(); return value;
  }
  size_t write(const uint8_t *data, size_t size) override {
    sent.emplace_back(data, data + size); return size;
  }
  void frame(uint8_t command, uint16_t arg = 0) {
    uint8_t data[10] = {0x7e, 0xff, 0x06, command, 0,
      static_cast<uint8_t>(arg >> 8), static_cast<uint8_t>(arg), 0, 0, 0xef};
    uint16_t sum = 0;
    for (unsigned i = 1; i < 7; ++i) sum += data[i];
    sum = static_cast<uint16_t>(-sum); data[7] = sum >> 8; data[8] = sum;
    for (uint8_t value : data) incoming.push_back(value);
  }
};
struct Fixture {
  ScriptedStream stream;
  DFRobotDFPlayerMini player;
  explicit Fixture(uint32_t start = 0) {
    nowMs = start; elapsed = 0;
    check(player.begin(stream, true, false), "begin without reset");
    player.setTimeOut(500);
  }
  void play(unsigned track = 7) { player.playLargeFolder(1, track); }
  void assertLast(unsigned track) {
    check(!stream.sent.empty(), "command transmitted");
    const auto &data = stream.sent.back();
    check(data.size() == 10 && data[3] == 0x14 && data[4] == 1 &&
          data[5] == 0x10 && data[6] == track, "correct new frame and ACK stays enabled");
  }
};
static void normalAck() {
  Fixture f; f.play(); f.stream.frame(0x41); f.play(8);
  check(elapsed < 50 && f.stream.sent.size() == 2, "normal ACK does not wait full timeout");
  f.assertLast(8);
}
static void missingAck(unsigned event) {
  Fixture f; f.play();
  if (event) f.stream.frame(event, 7);
  f.play(8);
  check(elapsed >= 500 && elapsed <= 510, "missing ACK wait is bounded at configured timeout");
  check(f.stream.sent.size() == 2, "next play transmitted after timeout");
  check(f.player.available() && f.player.readType() == TimeOut, "timeout is reported, never stale finished");
  f.assertLast(8);
}
static void retainedTimeout() {
  Fixture f; f.play(); f.play(8); // deliberately leave timeout unread
  const unsigned start = elapsed;
  f.play(9);
  check(elapsed-start >= 500 && elapsed-start <= 510, "retained timeout cannot trap next command");
  f.assertLast(9);
}
static void recovery() {
  Fixture f; f.play(); f.stream.frame(0x40, 1); f.play(8);
  check(f.player.available() && f.player.readType() == TimeOut, "failure event visible");
  f.stream.frame(0x41);
  const unsigned start = elapsed;
  f.play(9);
  check(elapsed-start < 50, "subsequent ACK restores prompt send");
  f.stream.frame(0x41); f.player.available();
  f.stream.frame(0x3d, 9);
  check(f.player.available() && f.player.readType() == DFPlayerPlayFinished && f.player.read() == 9,
        "new playback finish remains observable after recovery");
}
static void finishBeforeAck() {
  Fixture f; f.play(); f.stream.frame(0x3d, 7); f.stream.frame(0x41); f.play(8);
  check(elapsed < 50, "finish followed by ACK permits next play promptly");
  check(!f.player.available(), "finish collected during ACK wait cannot finish the new playback");
  f.stream.frame(0x3d, 8);
  check(f.player.available() && f.player.readType() == DFPlayerPlayFinished && f.player.read() == 8,
        "current playback finish is preserved");
}
static void errorBeforeAck() {
  Fixture f; f.play(); f.stream.frame(0x40, 1); f.stream.frame(0x41); f.play(8);
  check(elapsed < 50, "error followed by ACK permits next play promptly");
  check(f.player.available() && f.player.readType() == DFPlayerError && f.player.read() == 1,
        "non-finish event preserved when ACK succeeds");
}
static void rollover() {
  Fixture f(UINT32_MAX - 250); f.play(); f.stream.frame(0x3d, 7); f.play(8);
  check(nowMs < 1000 && elapsed >= 500 && elapsed <= 510, "ACK deadline works across millis rollover");
  check(f.player.available() && f.player.readType() == TimeOut, "rollover still reports timeout");
}
static void continuousNoise() {
  Fixture f; f.play(); f.stream.noise = true;
  f.player.available();
  check(f.stream.reads <= 10, "available has bounded UART byte budget");
  const unsigned start = elapsed; f.play(8);
  check(elapsed-start >= 1000 && elapsed-start <= 1020, "noise cannot hide ACK or pre-play deadlines");
  check(f.stream.sent.size() == 1, "flood skips new playback");
  f.stream.noise = false;
  check(f.player.available() && f.player.readType() == TimeOut, "noise reports timeout");
  f.stream.frame(0x41); f.play(9); f.assertLast(9);
}
static void waitAvailableNoise() {
  Fixture f(UINT32_MAX - 100); f.stream.noise = true;
  check(!f.player.waitAvailable(500), "waitAvailable noise timeout returns false");
  check(elapsed >= 500 && elapsed <= 510, "waitAvailable absolute timeout and rollover");
}
static void partialFrame() {
  Fixture f; f.stream.frame(0x3d, 8);
  std::deque<uint8_t> tail;
  while (f.stream.incoming.size() > 4) { tail.push_front(f.stream.incoming.back()); f.stream.incoming.pop_back(); }
  check(!f.player.available(), "partial frame waits without false event");
  f.stream.incoming = tail;
  check(f.player.available() && f.player.readType() == DFPlayerPlayFinished && f.player.read() == 8,
        "partial parser state survives calls");
}
static void noiseBeforeOldFinish() {
  Fixture f; f.play(); f.stream.frame(0x41); f.player.available();
  f.stream.incoming.push_back(0x00); f.stream.frame(0x3d, 7);
  f.play(8);
  check(f.stream.incoming.empty() && !f.player.hasPendingInput(), "old finish fully drained before play");
  check(!f.player.available(), "noise-prefixed old finish cannot complete new playback");
  f.assertLast(8);
}
static void truncatedPreplayRecovery() {
  Fixture f; f.stream.frame(0x3d, 7); f.stream.incoming.pop_back();
  f.play(8);
  check(elapsed >= 500 && elapsed <= 510 && f.stream.sent.empty(), "truncated pre-play frame skips play within deadline");
  check(!f.player.hasPendingInput(), "timeout clears truncated parser");
  f.play(9); f.assertLast(9);
}
APP_INTEGRATION
int main() {
  normalAck(); std::puts("PASS normal ACK");
  missingAck(0); std::puts("PASS missing ACK only");
  missingAck(0x3d); std::puts("PASS missing ACK plus finish");
  missingAck(0x40); std::puts("PASS missing ACK plus error");
  retainedTimeout(); std::puts("PASS retained timeout plus next missing ACK");
  recovery(); std::puts("PASS recovery and new playback finish");
  finishBeforeAck(); std::puts("PASS no stale finish after ACK wait");
  errorBeforeAck(); std::puts("PASS non-finish event preservation");
  rollover(); std::puts("PASS ACK deadline across rollover");
  continuousNoise(); std::puts("PASS continuous UART noise and recovery");
  waitAvailableNoise(); std::puts("PASS public event wait across rollover with noise");
  partialFrame(); std::puts("PASS partial frame parsing");
  noiseBeforeOldFinish(); std::puts("PASS noise-prefixed queued old finish is drained before playback");
  truncatedPreplayRecovery(); std::puts("PASS truncated pre-play frame timeout and recovery");
  applicationTests();
}
'''

APP_INTEGRATION = r'''

static ScriptedStream appStream;
static DFRobotDFPlayerMini myDFPlayer;
static bool dfPlayerReady = true;
#define MP3_WAIT_TIMEOUT_MS 2000
#define BREADCRUMB(x) ((void)0)
struct Printer { void println(const char*) {} } Serial;
APP_FUNCTIONS
static void resetApplication(uint32_t start=0) {
  nowMs=start; elapsed=0;
  appStream=ScriptedStream(); myDFPlayer=DFRobotDFPlayerMini(); dfPlayerReady=true;
  check(myDFPlayer.begin(appStream,true,false), "application begin");
  myDFPlayer.setTimeOut(500); Mp3BatteryCancel();
}
static void queueOldFinished(unsigned noise) {
  Mp3PlayLargeFolder(1,7); appStream.frame(0x41); myDFPlayer.available();
  for(unsigned i=0;i<noise;++i) appStream.incoming.push_back(0);
  appStream.frame(0x3d,7);
}
static void applicationTests() {
  for (unsigned noise : {1u, 10u, 81u}) {
    resetApplication(); queueOldFinished(noise); Mp3BatteryStart();
    while (batteryAudioPhase == BatteryAudioPhase::Preparing)
      check(!Mp3BatteryFinished(), "preparing/starting new battery playback is unfinished");
    check(appStream.sent.size()==2 && !Mp3BatteryFinished(), "old queued finish cannot immediately finish battery audio");
    appStream.frame(0x41); appStream.frame(0x3d,3);
    check(!Mp3BatteryFinished() && Mp3BatteryFinished(), "ACK then real new battery finish completes normally");
  }
  std::puts("PASS actual battery app: 1/10/81 noise bytes before old finish");
  resetApplication(); Mp3PlayLargeFolder(1,7); appStream.frame(0x3d,7);
  Mp3BatteryStart(); check(!Mp3BatteryFinished(), "missing ACK plus pending finish permits new battery play");
  check(elapsed>=500 && elapsed<600 && appStream.sent.size()==2, "actual app missing ACK remains bounded");
  check(!Mp3BatteryFinished(), "missing ACK old finish not reused");
  std::puts("PASS actual battery app: missing ACK plus old finish");
  resetApplication(UINT32_MAX-1000); appStream.frame(0x3d,7); appStream.incoming.pop_back();
  Mp3BatteryStart();
  check(!Mp3BatteryFinished() && appStream.sent.empty(), "partial old frame postpones playback");
  delay(2000); check(Mp3BatteryFinished() && appStream.sent.empty(), "partial old frame skips audio at two-second rollover-safe deadline");
  check(!myDFPlayer.hasPendingInput(), "app clears partial parser on timeout");
  Mp3BatteryStart(); check(!Mp3BatteryFinished() && appStream.sent.size()==1, "battery audio recovers after partial-frame timeout");
  std::puts("PASS actual battery app: partial frame deadline, rollover and recovery");
  resetApplication(); appStream.noise=true; Mp3BatteryStart();
  check(!Mp3BatteryFinished() && appStream.reads==80 && elapsed<=80, "flood processing has eight ten-byte calls per iteration");
  delay(2000); check(Mp3BatteryFinished() && appStream.sent.empty(), "flood skips battery audio at deadline");
  appStream.noise=false; Mp3BatteryStart();
  check(!Mp3BatteryFinished() && appStream.sent.size()==1, "battery recovers after flood stops");
  std::puts("PASS actual battery app: bounded flood and recovery");
  resetApplication(); queueOldFinished(1); Mp3TaggerStart();
  check(!Mp3TaggerFinished(), "old queued finish cannot complete tagger audio");
  appStream.frame(0x41); appStream.frame(0x3d,9);
  check(!Mp3TaggerFinished() && Mp3TaggerFinished(), "tagger ACK then real finish completes normally");
  std::puts("PASS actual tagger app: old queued finish and new completion");
  resetApplication(); queueOldFinished(1);
  unsigned before=elapsed; tickMillis=true; Mp3PlayLargeFolderAndWait(1,6); tickMillis=false;
  check(elapsed-before>=2000, "blocking play ignores old queued finish and reaches bounded timeout");
  std::puts("PASS actual blocking play app: old queued finish cannot skip wait");
  resetApplication();
  const unsigned begins=blackBoxTestTraces[GeneratorTrace::AudioBegin];
  const unsigned ends=blackBoxTestTraces[GeneratorTrace::AudioEnd];
  tickMillis=true; Mp3PlayLargeFolder(1,7); Mp3PlayLargeFolderAndWait(1,4); tickMillis=false;
  check(blackBoxTestTraces[GeneratorTrace::AudioBegin]==begins+2 &&
        blackBoxTestTraces[GeneratorTrace::AudioEnd]==ends+2, "send and blocking play each trace begin and end");
  dfPlayerReady=false; Mp3PlayLargeFolder(1,7); Mp3PlayLargeFolderAndWait(1,4);
  check(blackBoxTestTraces[GeneratorTrace::AudioBegin]==begins+2, "no audio trace when DFPlayer is absent");
  std::puts("PASS audio begin/end reach the flash trace; absent DFPlayer adds nothing");
}

'''

def application_harness():
    source = APP.read_text()
    def function(name):
        import re
        match = re.search(r'(?:void|bool) ' + name + r'\(', source)
        begin = match.start(); pos = source.index('{', begin); depth = 1; end = pos + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}'); end += 1
        return source[begin:end]
    functions = '\n'.join(function(name) for name in ('Mp3PlayLargeFolder', 'Mp3PlayLargeFolderAndWait'))
    functions += '\nstatic unsigned long taggerAudioStarted=0;\n'
    functions += '\n'.join(function(name) for name in ('Mp3TaggerStart', 'Mp3TaggerFinished'))
    functions += '\n' + source[source.index('enum class BatteryAudioPhase'):]
    return '#include "tests/diagnostic_test_stubs.h"\n' + APP_INTEGRATION.replace('APP_FUNCTIONS', functions)

def main():
    with tempfile.TemporaryDirectory(prefix='generator-dfplayer-test-') as temporary:
        directory = Path(temporary)
        (directory / 'Arduino.h').write_text(ARDUINO)
        (directory / 'test.cpp').write_text(HARNESS.replace('APP_INTEGRATION', application_harness()))
        executable = directory / 'test'
        compiler = os.environ.get('CXX', 'c++')
        compiler_version = subprocess.check_output([compiler, '--version'], text=True)
        # Upstream 1.0.6 keeps an unused private 'device' field.
        extra_flags = ['-Wno-unused-private-field'] if 'clang' in compiler_version else []
        subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', *extra_flags,
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(directory), '-I', str(APP.parent), '-I', str(LIBRARY), str(directory / 'test.cpp'),
                        str(LIBRARY / 'DFRobotDFPlayerMini.cpp'), '-o', str(executable)], check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=10)

if __name__ == '__main__':
    main()
