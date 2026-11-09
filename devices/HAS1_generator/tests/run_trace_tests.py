#!/usr/bin/env python3
"""Exercise the plug/audio trace buffer: quiet windows, pacing, raw coalescing, caps."""
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
struct Logged { Entry e; uint32_t at, age, extra; };
// Like BlackBoxTraceFlush(): at most one peek/commit per loop step (1 ms here).
static std::vector<Logged> drain(Buffer &b, uint32_t from, uint32_t to) {
  std::vector<Logged> out; Entry e; uint32_t age, extra;
  for (uint32_t now = from;; ++now) {
    if (b.peek(now, e, age, extra)) { b.commit(now); out.push_back({e, now, age, extra}); }
    if (now == to) break;
  }
  return out;
}
int main() {
  {
    Buffer b;
    b.add(AudioBegin, 1001, 0x10007, 0, 0);
    b.add(AudioEnd, 1021, 0x10007, 0, 20);
    assert(drain(b, 1021, 1120).empty() && !b.audioQuiet(1120, 100));
    auto out = drain(b, 1121, 1200);
    assert(out.size() == 2 && out[0].at == 1121 && out[0].age == 120 && out[1].at == 1141);
    puts("PASS nothing is logged within 100 ms after a DFPlayer call; records 20 ms apart");
  }
  {
    Buffer b;
    b.add(WirePresent, 1000, 4, 3, 0);
    b.add(AudioBegin, 1001, 0x10007, 0, 0);
    b.add(AudioEnd, 1021, 0x10007, 0, 20);
    b.add(WireAck, 1100, 4, 9, 1);              // the ack can start (1,3) next
    assert(drain(b, 1021, 1599).empty());
    auto out = drain(b, 1600, 1700);
    assert(out.size() == 4 && out[0].at == 1600 && out[0].e.kind == WirePresent && out[0].age == 600);
    assert(out[3].e.kind == WireAck && out[3].at == 1660);
    puts("PASS backlog waits 500 ms after the last count presentation/ack");
  }
  {
    Buffer b;
    b.add(WireRaw, 0, 1, 0, 0); b.add(WireRaw, 100, 0, 1, 0); b.add(WireRaw, 200, 1, 0, 0);
    auto out = drain(b, 300, 300);
    assert(out.size() == 1 && out[0].e.a == 1 && out[0].extra == 3 && out[0].age == 100);
    b.add(WireRaw, 310, 0, 1, 0);
    assert(drain(b, 301, 549).empty());         // 250 ms raw spacing
    out = drain(b, 550, 5000);                  // trailing edge: final release is logged once
    assert(out.size() == 1 && out[0].e.a == 0 && out[0].extra == 1 && out[0].at == 550);
    puts("PASS raw contact coalesces to the latest state; trailing edge always logged");
  }
  {
    Buffer b; unsigned logged = 0; uint32_t changes = 0, last = 0;
    for (uint32_t t = 0; t < 60000; t += 300) {
      b.add(WireRaw, t, int32_t(t / 300 % 2), 0, 0);
      for (auto &l : drain(b, t + 1, t + 299)) { ++logged; changes += l.extra; last = uint32_t(l.e.a); }
    }
    assert(logged == Buffer::RawPerWindow);
    auto out = drain(b, 60000, 60100);          // next window releases the held state
    assert(out.size() == 1);
    changes += out[0].extra; last = uint32_t(out[0].e.a);
    assert(changes == 200 && last == 1);
    puts("PASS chatter capped at 30 raw records per minute without losing count or final state");
  }
  {
    Buffer b;
    for (unsigned i = 0; i < 20; ++i) b.add(AudioEnd, i, int32_t(i), 0, 0);
    auto out = drain(b, 0, 2000);
    assert(out.size() == Buffer::Capacity && out[0].extra == 4 && out[0].e.a == 0);
    for (unsigned i = 1; i < out.size(); ++i) assert(out[i].extra == 0 && out[i].e.a == int32_t(i));
    puts("PASS queue overflow is bounded and counted on the next record");
  }
  {
    Buffer b;
    b.add(AudioEnd, 500, 2, 1, 0);
    b.add(WireRaw, 400, 3, 0, 0);
    auto out = drain(b, 600, 700);
    assert(out.size() == 2 && out[0].e.kind == WireRaw && out[1].e.kind == AudioEnd);
    puts("PASS raw and queued records are logged oldest first");
  }
  {
    Buffer b; Entry e; uint32_t age, extra;
    b.add(AudioEnd, 0, 7, 0, 0);
    assert(b.peek(200, e, age, extra) && e.a == 7);
    b.defer(200);                               // logger rejected it
    assert(!b.peek(219, e, age, extra));
    assert(b.peek(220, e, age, extra) && e.a == 7 && age == 220);
    b.commit(220);
    assert(!b.peek(1000, e, age, extra));
    b.commit(1000);                             // commit without a peek is a no-op
    puts("PASS a rejected record is offered again after the spacing, then committed once");
  }
  {
    Buffer b; const uint32_t t = 0xffffff00u;
    b.add(AudioEnd, t, 1, 0, 0);
    assert(drain(b, t + 1, t + 99).empty());
    auto out = drain(b, t + 400, t + 400);      // wraps past zero
    assert(out.size() == 1 && out[0].age == 400);
    b.add(WireRaw, t + 500, 1, 0, 0);
    assert(drain(b, t + 520, t + 520).size() == 1);
    puts("PASS millis rollover keeps quiet windows, spacing and age");
  }
  {
    Buffer b; b.add(Count, 0, 0, 0, 0); b.add(200, 0, 0, 0, 0);
    assert(drain(b, 0, 1000).empty());
    assert(packAgeExtra(70000, 1) == (0xffffu | (1u << 16)));
    assert(packAgeExtra(5, 40000) == (5u | (0x7fffu << 16)));
    assert(int32_t(packAgeExtra(0xffffffffu, 0xffffffffu)) > 0);
    puts("PASS unknown kinds ignored; age/extra packing saturates and stays positive");
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
