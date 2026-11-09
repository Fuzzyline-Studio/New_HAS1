#ifndef GENERATOR_TRACE_H
#define GENERATOR_TRACE_H
#include <stdint.h>

// Plug/audio evidence for the flash blackbox. Entries are captured in RAM on
// the main task and logged later, never next to DFPlayer UART traffic: the
// blackbox writer's flash access stalls the other core, which would corrupt a
// bit-banged 9600-baud DFPlayer frame or its ACK. Each record carries its age.
namespace GeneratorTrace {
enum Kind : uint8_t { WireRaw = 0, WirePresent, WireAck, AudioBegin, AudioEnd, Count };
struct Entry { uint32_t at; int32_t a, b, c; uint8_t kind; };

// Main task only (no locks). Fixed storage; unsigned time arithmetic tolerates
// millis rollover.
class Buffer {
 public:
  static const unsigned Capacity = 16;       // queued non-raw entries
  static const uint32_t QuietMs = 100;       // after the last audio call
  static const uint32_t RawSpacingMs = 250;  // between raw contact records
  static const uint32_t RawWindowMs = 60000;
  static const unsigned RawPerWindow = 30;

  void add(uint8_t kind, uint32_t now, int32_t a, int32_t b, int32_t c) {
    if (kind >= Count) return;
    if (kind == AudioBegin || kind == AudioEnd) { audioAt_ = now; audioSeen_ = true; }
    if (kind == WireRaw) {
      // Coalesce to the latest contact state. It is always logged eventually
      // (trailing edge) with the number of changes it stands for.
      raw_ = Entry{now, a, b, c, WireRaw}; rawPending_ = true; ++rawChanges_;
      return;
    }
    if (count_ == Capacity) { ++dropped_; return; }
    entries_[(head_ + count_) % Capacity] = Entry{now, a, b, c, kind};
    ++count_;
  }
  bool audioQuiet(uint32_t now, uint32_t ms) const {
    return !audioSeen_ || uint32_t(now - audioAt_) >= ms;
  }
  // One record ready to log, oldest first. `extra` is the coalesced raw change
  // count, or queued entries dropped since the previous queued record.
  bool next(uint32_t now, Entry &out, uint32_t &age, uint32_t &extra) {
    if (!audioQuiet(now, QuietMs)) return false;
    const bool rawReady = rawPending_ && rawAllowed(now);
    if (!count_ && !rawReady) return false;
    const bool takeRaw = rawReady && (!count_ || int32_t(raw_.at - entries_[head_].at) <= 0);
    if (takeRaw) {
      out = raw_; extra = rawChanges_;
      rawPending_ = false; rawChanges_ = 0; rawLastAt_ = now; rawSeen_ = true; ++rawInWindow_;
    } else {
      out = entries_[head_]; head_ = (head_ + 1) % Capacity; --count_;
      extra = dropped_; dropped_ = 0;
    }
    age = now - out.at;
    return true;
  }

 private:
  bool rawAllowed(uint32_t now) {
    if (!rawWindowSeen_ || uint32_t(now - rawWindowAt_) >= RawWindowMs) {
      rawWindowSeen_ = true; rawWindowAt_ = now; rawInWindow_ = 0;
    }
    if (rawInWindow_ >= RawPerWindow) return false;
    return !rawSeen_ || uint32_t(now - rawLastAt_) >= RawSpacingMs;
  }
  Entry entries_[Capacity] = {};
  Entry raw_ = {};
  unsigned head_ = 0, count_ = 0, rawInWindow_ = 0;
  uint32_t dropped_ = 0, rawChanges_ = 0, audioAt_ = 0, rawLastAt_ = 0, rawWindowAt_ = 0;
  bool audioSeen_ = false, rawPending_ = false, rawSeen_ = false, rawWindowSeen_ = false;
};
}  // namespace GeneratorTrace
#endif
