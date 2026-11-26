#pragma once
#include "BlackBoxRecord.h"
namespace has1blackbox {
struct Flash {
  virtual ~Flash() = default;
  virtual bool read(size_t, uint8_t *, size_t) = 0;
  virtual bool write(size_t, const uint8_t *, size_t) = 0;
  virtual bool erase(size_t, size_t) = 0;
  virtual size_t size() const = 0;
};
enum class AppendResult { Persisted, Failed, Uncertain, Full, Unavailable };
// Why the last begin() did or did not open the ring; numeric values are shown
// on device consoles, so only append.
enum class RingStatus : uint8_t {
  Unopened,         // begin() not run, or a fresh partition without identity
  Ready,
  BadSize,          // partition smaller than Size, above the cap or not
                    // whole sectors
  ReadError,
  MetadataConflict, // two ring identities, or one generation twice
  ForeignData,      // unknown content (possibly a file); left untouched
  EraseError,
  WriteError,
  RecordConflict,   // record beyond reservation or duplicate sequence
};
// Single owner. Sequence reservations leave intentional gaps after reset.
class Ring {
public:
  static constexpr size_t Sector = 4096, Size = 131072, Slots = 1920,
                          Data = 8192, MaxSize = 196608, LargeMaxSize = 0x200000;
  explicit Ring(Flash &f) : f_(f) {}
  // Largest accepted partition: MaxSize, or LargeMaxSize after the opt-in.
  static constexpr size_t maxSize(bool largePartition) {
    return largePartition ? LargeMaxSize : MaxSize;
  }
  // The ring occupies the first Size bytes. Up to MaxSize (the 192 KiB spiffs
  // of the Arduino 2.x min_spiffs layout) is accepted; the remainder is only
  // ever checked and, together with the ring, erased when unused. The cap
  // bounds that one-time erase and matches decode_flash.py.
  // largePartition (opt-in, default off) also accepts whole-sector sizes up to
  // LargeMaxSize, e.g. the 1.4 MiB spiffs of the default 4 MB scheme. Such a
  // partition is still proven unused as a whole before first use, but only
  // the ring's own Size bytes are ever erased or written; the rest stays as
  // it was.
  bool begin(uint64_t newId, bool largePartition = false) {
    ready_ = false;
    if (f_.size() < Size || f_.size() > maxSize(largePartition) ||
        f_.size() % Sector)
      return fail(RingStatus::BadSize);
    uint8_t best[64]{};
    uint32_t seenGeneration[128]{};
    bool found = false;
    size_t bestSector = 0;
    for (size_t slot = 0; slot < 128; ++slot) {
      uint8_t candidate[64];
      if (!f_.read(slot * 64, candidate, 64))
        return fail(RingStatus::ReadError);
      if (!metadataValid(candidate))
        continue;
      if (found && id(candidate) != id(best))
        return fail(RingStatus::MetadataConflict);
      const uint32_t gen = get32(candidate + 8);
      for (size_t previous = 0; previous < slot; ++previous) {
        if (seenGeneration[previous] != gen)
          continue;
        uint8_t old[64];
        if (!f_.read(previous * 64, old, 64))
          return fail(RingStatus::ReadError);
        if (std::memcmp(candidate, old, 64))
          return fail(RingStatus::MetadataConflict);
      }
      seenGeneration[slot] = gen;
      if (!found || gen > get32(best + 8)) {
        std::memcpy(best, candidate, 64);
        bestSector = slot / 64;
        found = true;
      }
    }
    if (!found) {
      size_t marker = 0;
      if (!unused(marker))
        return false;
      if (!newId)
        return fail(RingStatus::Unopened);
      // Holds no file, so erasing it cannot lose data; see unused(). Sector
      // erases keep each flash-cache stall short for the other core. A
      // partition up to MaxSize is erased whole. A larger one (accepted only
      // with largePartition) is erased only over the ring; the SPIFFS magics
      // left beyond it keep one block count, so unused() still accepts the
      // partition when an interrupted first initialization is retried.
      const size_t end = f_.size() > MaxSize ? Size : f_.size();
      for (size_t off = 0; marker < end && off < end; off += Sector)
        if (!f_.erase(off, Sector))
          return fail(RingStatus::EraseError);
      if (marker < end) {
        if (!unused(marker)) // keeps unused()'s ReadError/ForeignData
          return false;
        if (marker < end)
          return fail(RingStatus::EraseError);
      }
      ringId_ = newId;
      generation_ = 0;
      reserved_ = 1;
      acked_ = 0;
      active_ = 1;
      if (!checkpoint(1, 0))
        return fail(RingStatus::WriteError);
    } else {
      active_ = bestSector;
      ringId_ = id(best);
      generation_ = get32(best + 8);
      reserved_ = get32(best + 20);
      acked_ = get32(best + 24);
    }
    durableAck_ = acked_;
    next_ = reserved_;
    max_ = 0;
    cursor_ = 0;
    for (size_t i = 0; i < Slots; ++i) {
      Record r;
      if (!read(i, r))
        return fail(RingStatus::ReadError);
      seqs_[i] = r.valid() ? r.seq() : 0;
      dirty_[i] = !r.erased();
      if (!r.valid())
        continue;
      if (r.seq() >= reserved_)
        return fail(RingStatus::RecordConflict);
      for (size_t j = 0; j < i; ++j)
        if (seqs_[j] == r.seq())
          return fail(RingStatus::RecordConflict);
      if (r.seq() > max_) {
        max_ = r.seq();
        cursor_ = (i + 1) % Slots;
      }
    }
    if (acked_ >= reserved_)
      return fail(RingStatus::RecordConflict);
    ready_ = true;
    status_ = RingStatus::Ready;
    return true;
  }
  bool ready() const { return ready_; }
  RingStatus status() const { return status_; }
  uint64_t id() const { return ringId_; }
  uint32_t acked() const { return acked_; }
  uint32_t maximum() const { return max_; }
  uint32_t lostRecords() const { return lost_; }
  bool checkpointAck() {
    if (acked_ == durableAck_)
      return true;
    if (!checkpoint(reserved_, acked_)) {
      ready_ = false;
      return false;
    }
    return true;
  }
  AppendResult append(Record &r) {
    if (!ready_)
      return AppendResult::Unavailable;
    if (next_ == UINT32_MAX)
      return AppendResult::Unavailable;
    if (next_ >= reserved_) {
      uint32_t end = next_ > UINT32_MAX - 64 ? UINT32_MAX : next_ + 64;
      if (!checkpoint(end, acked_)) {
        ready_ = false;
        return AppendResult::Failed;
      }
    }
    size_t chosen = Slots;
    for (size_t k = 0; k < Slots; ++k) {
      size_t i = (cursor_ + k) % Slots;
      if (!dirty_[i]) {
        chosen = i;
        break;
      }
      if (i % 64 == 0) {
        bool safe = true;
        for (size_t j = i; j < i + 64; ++j)
          if (seqs_[j] > acked_)
            safe = false;
        if (safe) {
          if (!checkpointAck())
            return AppendResult::Failed;
          if (!f_.erase(Data + i * 64, Sector)) {
            ready_ = false;
            return AppendResult::Uncertain;
          }
          Record check;
          for (size_t j = i; j < i + 64; ++j) {
            if (!read(j, check) || !check.erased()) {
              ready_ = false;
              return AppendResult::Uncertain;
            }
            dirty_[j] = false;
            seqs_[j] = 0;
          }
          chosen = i;
          break;
        }
      }
    }
    if (chosen == Slots) {
      // Rolling window: reclaim the oldest sector, preserving a loss marker
      // when possible.
      size_t oldest = 0;
      uint32_t first = UINT32_MAX, last = 0, count = 0;
      for (size_t i = 0; i < Slots; ++i)
        if (seqs_[i] && seqs_[i] < first) {
          first = seqs_[i];
          oldest = (i / 64) * 64;
        }
      first = UINT32_MAX;
      for (size_t i = oldest; i < oldest + 64; ++i)
        if (seqs_[i] > acked_) {
          if (seqs_[i] < first)
            first = seqs_[i];
          if (seqs_[i] > last)
            last = seqs_[i];
          ++count;
        }
      lost_ += count;
      if (!f_.erase(Data + oldest * 64, Sector)) {
        ready_ = false;
        return AppendResult::Uncertain;
      }
      Record check;
      for (size_t i = oldest; i < oldest + 64; ++i) {
        if (!read(i, check) || !check.erased()) {
          ready_ = false;
          return AppendResult::Uncertain;
        }
        dirty_[i] = false;
        seqs_[i] = 0;
      }
      cursor_ = oldest;
      Record loss =
          Record::make(EventType::Lost, get32(r.bytes + 8), get32(r.bytes + 12),
                       int32_t(first), int32_t(last), int32_t(count), 0,
                       "oldest-sector-overwrite");
      auto result = append(loss);
      if (result != AppendResult::Persisted)
        return result;
      return append(r);
    }
    r.sequence(next_++);
    dirty_[chosen] = true;
    cursor_ = (chosen + 1) % Slots;
    bool wrote = f_.write(Data + chosen * 64, r.bytes, 64);
    Record check;
    if (!read(chosen, check)) {
      ready_ = false;
      return AppendResult::Uncertain;
    }
    if (check.valid()) {
      seqs_[chosen] = check.seq();
      if (check.seq() > max_)
        max_ = check.seq();
    }
    if (!wrote || std::memcmp(check.bytes, r.bytes, 64) != 0)
      return AppendResult::Uncertain;
    return AppendResult::Persisted;
  }
  bool acknowledge(uint32_t seq) {
    if (!ready_ || seq < acked_ || seq > max_)
      return false;
    acked_ = seq;
    return acked_ - durableAck_ < 64 || checkpointAck();
  }
  bool snapshot(Record *out, size_t cap, size_t &count) {
    count = 0;
    if (!ready_)
      return false;
    uint32_t after = acked_;
    while (count < cap) {
      size_t best = Slots;
      uint32_t lowest = UINT32_MAX;
      for (size_t i = 0; i < Slots; ++i)
        if (seqs_[i] > after && seqs_[i] < lowest) {
          lowest = seqs_[i];
          best = i;
        }
      if (best == Slots)
        break;
      if (!read(best, out[count]) || !out[count].valid() ||
          out[count].seq() != lowest)
        return false;
      after = lowest;
      ++count;
    }
    return true;
  }

private:
  Flash &f_;
  uint32_t seqs_[Slots]{};
  bool dirty_[Slots]{};
  bool ready_ = false;
  RingStatus status_ = RingStatus::Unopened;
  uint64_t ringId_ = 0;
  uint32_t generation_ = 0, reserved_ = 1, next_ = 1, acked_ = 0,
           durableAck_ = 0, max_ = 0, lost_ = 0;
  size_t active_ = 1, cursor_ = 0;
  bool fail(RingStatus status) {
    status_ = status;
    return false;
  }
  bool read(size_t i, Record &r) { return f_.read(Data + i * 64, r.bytes, 64); }
  // True for an entirely 0xff partition, or one Arduino formatted as an
  // empty SPIFFS (e.g. under the older 192 KiB min_spiffs layout): each 4 KiB
  // block then holds only its lookup-page magic, SPIFFS_MAGIC = 0x20140529 ^
  // 256-byte page ^ (blockCount - block) as uint16 at byte 252, plus the
  // erase count after it. Any other byte may belong to a file. Scans the whole
  // partition, including any part beyond the ring. marker is the offset of the
  // first sector holding a magic, or size() when there is none. Blank blocks
  // are skipped, so a partly erased SPIFFS still yields one consistent count.
  bool unused(size_t &marker) {
    marker = f_.size();
    uint32_t blockCount = 0;
    for (size_t sector = 0; sector < f_.size() / Sector; ++sector) {
      uint8_t page[256], tail[4] = {255, 255, 255, 255};
      for (size_t off = 0; off < Sector; off += sizeof(page)) {
        if (!f_.read(sector * Sector + off, page, sizeof(page)))
          return fail(RingStatus::ReadError);
        for (size_t i = 0; i < sizeof(page); ++i) {
          if (page[i] == 255)
            continue;
          if (off != 0 || i < 252)
            return fail(RingStatus::ForeignData);
          tail[i - 252] = page[i];
        }
      }
      const uint16_t magic = uint16_t(tail[0] | (tail[1] << 8));
      if (magic == 0xffff) {
        if (tail[2] != 255 || tail[3] != 255)
          return fail(RingStatus::ForeignData);
        continue;
      }
      // magic ^ (0x20140529 ^ 256) truncated = blockCount - block, nonzero.
      const uint16_t remaining = uint16_t(magic ^ 0x0429);
      const uint32_t count = uint32_t(remaining + sector);
      if (!remaining || (blockCount && blockCount != count))
        return fail(RingStatus::ForeignData);
      blockCount = count;
      if (marker == f_.size())
        marker = sector * Sector;
    }
    return true;
  }
  static uint64_t id(const uint8_t *p) {
    return uint64_t(get32(p + 12)) | (uint64_t(get32(p + 16)) << 32);
  }
  static bool metadataValid(const uint8_t *p) {
    return !std::memcmp(p, "BBR1", 4) && get32(p + 4) == 1 &&
           get32(p + 28) == Slots && get32(p + 32) == 0x434f4d31 &&
           id(p) != 0 && get32(p + 8) > 0 && get32(p + 20) > 0 &&
           get32(p + 24) < get32(p + 20) && get32(p + 60) == crc32(p, 60);
  }
  bool checkpoint(uint32_t reserve, uint32_t ack) {
    if (generation_ == UINT32_MAX)
      return false;
    uint8_t p[64]{};
    std::memcpy(p, "BBR1", 4);
    put32(p + 4, 1);
    put32(p + 8, generation_ + 1);
    put32(p + 12, uint32_t(ringId_));
    put32(p + 16, uint32_t(ringId_ >> 32));
    put32(p + 20, reserve);
    put32(p + 24, ack);
    put32(p + 28, Slots);
    put32(p + 32, 0x434f4d31);
    put32(p + 60, crc32(p, 60));
    const size_t target = 1 - active_;
    size_t offset = Data;
    uint8_t check[64];
    for (size_t slot = 0; slot < 64; ++slot) {
      size_t candidate = target * Sector + slot * 64;
      if (!f_.read(candidate, check, 64))
        return false;
      bool erased = true;
      for (auto value : check)
        if (value != 255) {
          erased = false;
          break;
        }
      if (erased) {
        offset = candidate;
        break;
      }
    }
    if (offset == Data) {
      // Only the inactive sector is reclaimed. The latest valid generation
      // remains intact in the other sector throughout erase/program/readback.
      if (!f_.erase(target * Sector, Sector))
        return false;
      for (size_t slot = 0; slot < 64; ++slot) {
        if (!f_.read(target * Sector + slot * 64, check, 64))
          return false;
        for (auto value : check)
          if (value != 255)
            return false;
      }
      offset = target * Sector;
    }
    if (!f_.write(offset, p, 64) || !f_.read(offset, check, 64) ||
        std::memcmp(p, check, 64))
      return false;
    active_ = target;
    ++generation_;
    reserved_ = reserve;
    acked_ = ack;
    durableAck_ = ack;
    return true;
  }
};
} // namespace has1blackbox
