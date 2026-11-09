#ifndef GENERATOR_TRACE_H
#define GENERATOR_TRACE_H
#include <stdint.h>

// Plug/audio evidence for the flash blackbox. Entries are captured in RAM on
// the main task and logged later: the blackbox writer's flash access stalls the
// other core, which would corrupt a bit-banged 9600-baud DFPlayer frame or its
// ACK. Records go out only at least 100 ms after the last DFPlayer call and
// 500 ms after the last count presentation/ack (which can trigger the next
// sound), one per 20 ms. A sound that starts later can still meet an ordinary
// blackbox write, as before this trace existed. Each record carries its age.
namespace GeneratorTrace {
enum Kind : uint8_t { WireRaw = 0, WirePresent, WireAck, AudioBegin, AudioEnd, Count };
struct Entry { uint32_t at; int32_t a, b, c; uint8_t kind; };

// Fourth logged value: age in ms (bits 0-15, saturated) | extra << 16 (saturated).
inline uint32_t packAgeExtra(uint32_t age, uint32_t extra) {
  return (age > 0xffffu ? 0xffffu : age) | ((extra > 0x7fffu ? 0x7fffu : extra) << 16);
}

// Main task only (no locks). Fixed storage; unsigned time arithmetic tolerates
// millis rollover.
class Buffer {
 public:
  static const unsigned Capacity = 16;       // queued non-raw entries
  static const uint32_t QuietMs = 100;       // after the last audio call
  static const uint32_t WireQuietMs = 500;   // after a count presentation/ack
  static const uint32_t SpacingMs = 20;      // between logged records
  static const uint32_t RawSpacingMs = 250;  // between raw contact records
  static const uint32_t RawWindowMs = 60000;
  static const unsigned RawPerWindow = 30;

  void add(uint8_t kind, uint32_t now, int32_t a, int32_t b, int32_t c) {
    if (kind >= Count) return;
    if (kind == AudioBegin || kind == AudioEnd) { audioAt_ = now; audioSeen_ = true; }
    if (kind == WirePresent || kind == WireAck) { wireAt_ = now; wireSeen_ = true; }
    if (kind == WireRaw) {
      // Coalesce to the latest contact state. It is always logged eventually
      // (trailing edge) with the number of changes it stands for. Raw changes
      // do not extend the quiet windows, so chatter cannot block flushing.
      raw_ = Entry{now, a, b, c, WireRaw}; rawPending_ = true; ++rawChanges_;
      return;
    }
    if (count_ >= Capacity) { ++dropped_; return; }
    entries_[(head_ + count_) % Capacity] = Entry{now, a, b, c, kind};
    ++count_;
  }
  bool audioQuiet(uint32_t now, uint32_t ms) const { return after(audioSeen_, audioAt_, now, ms); }
  // The record ready to log, oldest first, without removing it. Call commit()
  // once the logger accepted it, or defer() to offer it again after SpacingMs.
  // `extra` is the coalesced raw change count, or queued entries dropped since
  // the previous queued record.
  bool peek(uint32_t now, Entry &out, uint32_t &age, uint32_t &extra) {
    peeked_ = None;
    if (!audioQuiet(now, QuietMs) || !after(wireSeen_, wireAt_, now, WireQuietMs) ||
        !after(loggedSeen_, loggedAt_, now, SpacingMs)) return false;
    const bool rawReady = rawPending_ && rawAllowed(now);
    if (!count_ && !rawReady) return false;
    if (rawReady && (!count_ || int32_t(raw_.at - entries_[head_].at) <= 0)) {
      out = raw_; extra = rawChanges_; peeked_ = Raw;
    } else {
      out = entries_[head_]; extra = dropped_; peeked_ = Queued;
    }
    age = now - out.at;
    return true;
  }
  void commit(uint32_t now) {
    if (peeked_ == Raw) {
      rawPending_ = false; rawChanges_ = 0; rawLastAt_ = now; rawSeen_ = true; ++rawInWindow_;
    } else if (peeked_ == Queued) {
      head_ = (head_ + 1) % Capacity; --count_; dropped_ = 0;
    } else {
      return;
    }
    peeked_ = None; loggedAt_ = now; loggedSeen_ = true;
  }
  void defer(uint32_t now) { peeked_ = None; loggedAt_ = now; loggedSeen_ = true; }

 private:
  enum Peeked : uint8_t { None, Raw, Queued };
  static bool after(bool seen, uint32_t at, uint32_t now, uint32_t ms) {
    return !seen || uint32_t(now - at) >= ms;
  }
  bool rawAllowed(uint32_t now) {
    if (!rawWindowSeen_ || uint32_t(now - rawWindowAt_) >= RawWindowMs) {
      rawWindowSeen_ = true; rawWindowAt_ = now; rawInWindow_ = 0;
    }
    if (rawInWindow_ >= RawPerWindow) return false;
    return after(rawSeen_, rawLastAt_, now, RawSpacingMs);
  }
  Entry entries_[Capacity] = {};
  Entry raw_ = {};
  unsigned head_ = 0, count_ = 0, rawInWindow_ = 0;
  uint32_t dropped_ = 0, rawChanges_ = 0, audioAt_ = 0, wireAt_ = 0, loggedAt_ = 0;
  uint32_t rawLastAt_ = 0, rawWindowAt_ = 0;
  bool audioSeen_ = false, wireSeen_ = false, loggedSeen_ = false;
  bool rawPending_ = false, rawSeen_ = false, rawWindowSeen_ = false;
  Peeked peeked_ = None;
};
}  // namespace GeneratorTrace
#endif
