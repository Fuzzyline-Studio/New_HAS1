#!/usr/bin/env python3
"""Exercise the plug/audio trace buffer: audio quiet window, raw coalescing, caps."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include <cassert>
#include <cstdio>
#include <vector>
#include "generator_trace.h"
using namespace GeneratorTrace;
struct Logged { Entry e; uint32_t age, extra; };
static std::vector<Logged> drain(Buffer &b, uint32_t now) {
  std::vector<Logged> out; Entry e; uint32_t age, extra;
  while (b.next(now, e, age, extra)) out.push_back({e, age, extra});
  return out;
}
int main() {
  {
    Buffer b;
    b.add(WirePresent, 1000, 1, 0, 0);
    b.add(AudioBegin, 1001, 0x10007, 0, 0);
    b.add(AudioEnd, 1021, 0x10007, 0, 20);
    assert(drain(b, 1120).empty() && !b.audioQuiet(1120, 100));
    auto out = drain(b, 1121);
    assert(out.size() == 3 && out[0].e.kind == WirePresent && out[0].age == 121);
    assert(out[1].e.kind == AudioBegin && out[2].e.kind == AudioEnd && out[2].e.c == 20);
    puts("PASS nothing is logged within 100 ms of a DFPlayer call; ages keep event time");
  }
  {
    Buffer b;
    b.add(WireRaw, 0, 1, 0, 0); b.add(WireRaw, 100, 0, 1, 0); b.add(WireRaw, 200, 1, 0, 0);
    auto out = drain(b, 300);
    assert(out.size() == 1 && out[0].e.a == 1 && out[0].extra == 3 && out[0].age == 100);
    b.add(WireRaw, 310, 0, 1, 0);
    assert(drain(b, 400).empty());              // 250 ms spacing
    out = drain(b, 550);                        // trailing edge: final release is logged
    assert(out.size() == 1 && out[0].e.a == 0 && out[0].extra == 1);
    assert(drain(b, 5000).empty());             // no repeats without a change
    puts("PASS raw contact coalesces to the latest state; trailing edge always logged");
  }
  {
    Buffer b; unsigned logged = 0; uint32_t changes = 0, last = 0;
    for (uint32_t t = 0; t < 60000; t += 300) {
      b.add(WireRaw, t, int32_t(t / 300 % 2), 0, 0);
      for (auto &l : drain(b, t + 1)) { ++logged; changes += l.extra; last = uint32_t(l.e.a); }
    }
    assert(logged == Buffer::RawPerWindow);
    auto out = drain(b, 60001);                 // next window releases the held state
    assert(out.size() == 1);
    changes += out[0].extra; last = uint32_t(out[0].e.a);
    assert(changes == 200 && last == 1);
    puts("PASS chatter capped at 30 raw records per minute without losing count or final state");
  }
  {
    Buffer b;
    for (unsigned i = 0; i < 20; ++i) b.add(WirePresent, i, int32_t(i), 0, 0);
    auto out = drain(b, 1000);
    assert(out.size() == Buffer::Capacity && out[0].extra == 4 && out[0].e.a == 0);
    for (unsigned i = 1; i < out.size(); ++i) assert(out[i].extra == 0 && out[i].e.a == int32_t(i));
    puts("PASS queue overflow is bounded and counted on the next record");
  }
  {
    Buffer b;
    b.add(WirePresent, 500, 2, 1, 0);
    b.add(WireRaw, 400, 3, 0, 0);
    auto out = drain(b, 700);
    assert(out.size() == 2 && out[0].e.kind == WireRaw && out[1].e.kind == WirePresent);
    puts("PASS raw and queued records are logged oldest first");
  }
  {
    Buffer b; const uint32_t t = 0xffffff00u;
    b.add(AudioEnd, t, 1, 0, 0);
    assert(drain(b, t + 50).empty());
    auto out = drain(b, t + 400);               // wraps past zero
    assert(out.size() == 1 && out[0].age == 400);
    b.add(WireRaw, t + 500, 1, 0, 0);
    assert(drain(b, t + 520).size() == 1);
    puts("PASS millis rollover keeps quiet window, spacing and age");
  }
  {
    Buffer b; b.add(Count, 0, 0, 0, 0); b.add(200, 0, 0, 0, 0);
    assert(drain(b, 1000).empty());
    puts("PASS unknown kinds are ignored");
  }
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='generator-trace-test-') as folder:
    d = Path(folder)
    (d / 'test.cpp').write_text(TEST)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(ROOT),
                    str(d / 'test.cpp'), '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
