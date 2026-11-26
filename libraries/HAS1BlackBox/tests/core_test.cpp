#include "BlackBoxEvidence.h"
#include "BlackBoxIdentity.h"
#include <type_traits>
static_assert(std::is_trivial<has1blackbox::RetainedEvidence>::value,"RTC no-init storage must have no startup constructor");
#include "BlackBoxRing.h"
#include "BlackBoxUpload.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>
using namespace has1blackbox;
struct Memory : Flash {
  std::vector<uint8_t> data = std::vector<uint8_t>(Ring::Size, 255);
  int writeLimit = -1, eraseLimit = -1;
  bool failRead = false, writeDataOnly = false, eraseDataOnly = false;
  unsigned writes = 0, erases = 0, metadataWrites = 0, metadataErases = 0;
  unsigned metadataSectorErases[2]{};
  size_t size() const override { return data.size(); }
  bool read(size_t o, uint8_t *b, size_t n) override {
    if (failRead || o + n > data.size())
      return false;
    std::memcpy(b, data.data() + o, n);
    return true;
  }
  bool write(size_t o, const uint8_t *b, size_t n) override {
    ++writes;
    if (o < Ring::Data)
      ++metadataWrites;
    bool fault = writeLimit >= 0 && (!writeDataOnly || o >= Ring::Data);
    size_t count = fault ? std::min(n, size_t(writeLimit)) : n;
    if (count)
      for (size_t i = 0; i < n; ++i)
        assert(data[o + i] == 255); // never reprogram a dirty slot
    for (size_t i = 0; i < count; ++i) {
      assert((data[o + i] & b[i]) == b[i]);
      data[o + i] &= b[i];
    }
    return !fault;
  }
  bool erase(size_t o, size_t n) override {
    ++erases;
    if (o < Ring::Data) {
      ++metadataErases;
      ++metadataSectorErases[o / Ring::Sector];
    }
    assert(o % 4096 == 0 && n % 4096 == 0);
    bool fault = eraseLimit >= 0 && (!eraseDataOnly || o >= Ring::Data);
    size_t count = fault ? std::min(n, size_t(eraseLimit)) : n;
    std::fill(data.begin() + o, data.begin() + o + count, 255);
    return !fault;
  }
};
Record event() {
  return Record::make(EventType::Health, 0x12345678, 0xfffffff0, Unknown, 2, 3,
                      4, "hello\nworld");
}
void records() {
  assert(crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) ==
         0xcbf43926);
  Record r = event();
  assert(!r.valid());
  r.sequence(1);
  assert(r.valid());
  assert(get32(r.bytes + 8) == 0x12345678);
  assert(get32(r.bytes + 16) == 0x80000000);
  assert(r.bytes[37] == '?');
  for (size_t i = 0; i < 64; ++i) {
    auto corrupt = r;
    corrupt.bytes[i] ^= 1;
    assert(!corrupt.valid());
  }
  auto longText = Record::make(EventType::State, 0, 0, 0, 0, 0, 0,
                               "abcdefghijklmnopqrstuvwxyz0123456789");
  assert(longText.bytes[59] == 0);
  char h[129];
  assert(r.hex(h, sizeof(h)));
  assert(!r.hex(h, 128));
  Record erased;
  std::memset(erased.bytes, 255, 64);
  assert(erased.erased());
  assert(!erased.valid());
  assert(uint32_t(0x10u - 0xfffffff0u) == 32);
}
void recovery() {
  Memory f;
  Ring r(f);
  assert(r.begin(0x1234));
  auto e = event();
  assert(r.append(e) == AppendResult::Persisted);
  assert(e.seq() == 1);
  Ring reboot(f);
  assert(reboot.begin(0x9999));
  assert(reboot.id() == 0x1234);
  e = event();
  assert(reboot.append(e) == AppendResult::Persisted);
  assert(e.seq() == 65);
  assert(reboot.acknowledge(65));
  Ring next(f);
  assert(next.begin(0));
  assert(next.id() == 0x1234 && next.acked() == 65);
  Record out[16];
  size_t count;
  assert(next.snapshot(out, 16, count) && count == 0);
  assert(!next.acknowledge(66));
}
void fullAndRecycle() {
  Memory f;
  Ring r(f);
  assert(r.begin(0x42));
  Record e;
  for (size_t i = 0; i < Ring::Slots; ++i) {
    e = event();
    assert(r.append(e) == AppendResult::Persisted);
    assert(e.seq() == i + 1);
  }
  e = event();
  assert(r.append(e) == AppendResult::Persisted);
  assert(e.seq() == 1922);
  assert(r.lostRecords() == 64);
  Record loss;
  assert(f.read(Ring::Data, loss.bytes, 64));
  assert(loss.valid() && loss.bytes[1] == uint8_t(EventType::Lost));
  assert(get32(loss.bytes + 16) == 1 && get32(loss.bytes + 20) == 64 &&
         get32(loss.bytes + 24) == 64);
  Ring reboot(f);
  assert(reboot.begin(0));
  Record out[16];
  size_t count;
  assert(reboot.snapshot(out, 16, count) && count == 16 && out[0].seq() == 65);
  assert(reboot.acknowledge(reboot.maximum()));
  for (size_t i = 0; i < 1920; ++i) {
    e = event();
    assert(reboot.append(e) == AppendResult::Persisted);
  }
  // Uncheckpointed small ACKs replay safely after reset; explicit checkpoint
  // makes them durable.
  Memory cf;
  Ring cr(cf);
  assert(cr.begin(2));
  e = event();
  assert(cr.append(e) == AppendResult::Persisted);
  assert(cr.acknowledge(1));
  Ring replay(cf);
  assert(replay.begin(0) && replay.acked() == 0);
  assert(cr.checkpointAck());
  Ring confirmed(cf);
  assert(confirmed.begin(0) && confirmed.acked() == 1);
}
void tornRecords() {
  for (int cut = 0; cut <= 64; ++cut) {
    Memory f;
    Ring r(f);
    assert(r.begin(42));
    auto e = event();
    assert(r.append(e) == AppendResult::Persisted);
    f.writeLimit = cut;
    e = event();
    assert(r.append(e) == AppendResult::Uncertain);
    f.writeLimit = -1;
    Ring recovered(f);
    assert(recovered.begin(0));
    e = event();
    assert(recovered.append(e) == AppendResult::Persisted);
    assert(e.seq() == 65);
    Record out[16];
    size_t count;
    assert(recovered.snapshot(out, 16, count));
    assert(out[0].seq() == 1);
    if (cut == 64) {
      assert(count == 3 && out[1].seq() == 2);
    } else
      assert(count == 2);
  }
}
void tornMetadata() {
  for (int cut = 0; cut <= 64; ++cut) {
    Memory f;
    Ring r(f);
    assert(r.begin(42));
    Record e;
    for (int i = 0; i < 64; ++i) {
      e = event();
      assert(r.append(e) == AppendResult::Persisted);
    }
    f.writeLimit = cut;
    e = event();
    assert(r.append(e) == AppendResult::Failed);
    f.writeLimit = -1;
    Ring recovered(f);
    assert(recovered.begin(0));
    e = event();
    assert(recovered.append(e) == AppendResult::Persisted);
    assert(e.seq() == (cut == 64 ? 129u : 65u));
  }
  for (int cut = 0; cut <= 64; ++cut) {
    Memory f;
    Ring r(f);
    f.writeLimit = cut;
    assert(!r.begin(42));
    f.writeLimit = -1;
    Ring recovered(f);
    bool ok = recovered.begin(99);
    assert(ok == (cut == 0 || cut == 64));
    if (cut == 64)
      assert(recovered.id() == 42);
  }
  for (int cut = 0; cut <= 64; ++cut) {
    Memory f;
    Ring r(f);
    assert(r.begin(42));
    auto e = event();
    assert(r.append(e) == AppendResult::Persisted);
    assert(r.acknowledge(1));
    f.writeLimit = cut;
    assert(!r.checkpointAck());
    f.writeLimit = -1;
    Ring recovered(f);
    assert(recovered.begin(0));
    assert(recovered.acked() == (cut == 64 ? 1u : 0u));
  }
}
void partialErase() {
  for (int cut : {0, 1, 63, 64, 2048, 4095, 4096}) {
    Memory f;
    Ring r(f);
    assert(r.begin(42));
    Record e;
    for (size_t i = 0; i < Ring::Slots; ++i) {
      e = event();
      assert(r.append(e) == AppendResult::Persisted);
    }
    assert(r.acknowledge(64));
    // Ensure sequence reservation checkpoint is already available before
    // faulting data erase.
    e = event();
    assert(r.append(e) == AppendResult::Persisted); // erased ACKed sector, now
                                                    // occupies first slot
    assert(r.acknowledge(e.seq()));
    for (int i = 1; i < 64; ++i) {
      e = event();
      assert(r.append(e) == AppendResult::Persisted);
    }
    // Next append may checkpoint first; any interrupted erase must leave
    // recoverable evidence.
    f.eraseLimit = cut;
    e = event();
    auto result = r.append(e);
    assert(result != AppendResult::Persisted);
    f.eraseLimit = -1;
    Ring recovered(f);
    assert(recovered.begin(0));
    Record out[16];
    size_t n;
    assert(recovered.snapshot(out, 16, n));
  }
}
void rollingPowerCuts() {
  Memory baseline;
  Ring full(baseline);
  assert(full.begin(0x42));
  for (size_t i = 0; i < Ring::Slots; ++i) {
    auto e = event();
    assert(full.append(e) == AppendResult::Persisted);
  }
  for (int cut = 0; cut <= 64; ++cut) {
    Memory f = baseline;
    Ring r(f);
    assert(r.begin(0));
    f.writeDataOnly = true;
    f.writeLimit = cut;
    auto e = event();
    assert(r.append(e) == AppendResult::Uncertain);
    f.writeLimit = -1;
    Ring recovered(f);
    assert(recovered.begin(0));
    e = event();
    assert(recovered.append(e) == AppendResult::Persisted);
    assert(e.seq() >= 1985);
    Record out[16];
    size_t count;
    assert(recovered.snapshot(out, 16, count));
    assert(out[0].seq() == 65);
  }
  for (int cut : {0, 1, 63, 64, 2048, 4095, 4096}) {
    Memory f = baseline;
    Ring r(f);
    assert(r.begin(0));
    f.eraseDataOnly = true;
    f.eraseLimit = cut;
    auto e = event();
    assert(r.append(e) == AppendResult::Uncertain);
    f.eraseLimit = -1;
    Ring recovered(f);
    assert(recovered.begin(0));
    e = event();
    assert(recovered.append(e) == AppendResult::Persisted);
    assert(e.seq() >= 1985);
  }
}
void metadataJournal() {
  Memory f;
  Ring r(f);
  assert(r.begin(77));
  for (unsigned i = 0; i < 4096; ++i) {
    auto e = event();
    assert(r.append(e) == AppendResult::Persisted);
    assert(r.acknowledge(e.seq()));
    assert(r.checkpointAck());
  }
  assert(f.metadataWrites >= 4096);
  assert(f.metadataErases > 0 && f.metadataWrites / f.metadataErases >= 64);
  for (auto count : f.metadataSectorErases)
    assert(count >= 31 && count <= 33);
  Ring recovered(f);
  assert(recovered.begin(0));
  assert(recovered.acked() == r.acked());
  std::cout << "metadata journal: " << f.metadataWrites << " writes, "
            << f.metadataErases << " erases (" << f.metadataSectorErases[0]
            << "/" << f.metadataSectorErases[1] << ")\n";

  // Fill both 64-entry metadata sectors using one new reservation per reboot.
  Memory full;
  {
    Ring first(full);
    assert(first.begin(77));
    auto e = event();
    assert(first.append(e) == AppendResult::Persisted);
  }
  while (full.metadataWrites < 128) {
    Ring boot(full);
    assert(boot.begin(0));
    auto e = event();
    assert(boot.append(e) == AppendResult::Persisted);
  }
  assert(full.metadataWrites == 128 && full.metadataErases == 0);
  for (int cut = 0; cut <= 64; ++cut) {
    Memory broken = full;
    Ring boot(broken);
    assert(boot.begin(0));
    broken.writeLimit = cut;
    auto e = event();
    assert(boot.append(e) == AppendResult::Failed);
    broken.writeLimit = -1;
    assert(broken.metadataErases == 1);
    Ring reboot(broken);
    assert(reboot.begin(0));
    e = event();
    assert(reboot.append(e) == AppendResult::Persisted);
    assert(e.seq() == (cut == 64 ? 8193u : 8129u));
  }
  for (int cut : {0, 1, 63, 64, 2048, 4095, 4096}) {
    Memory broken = full;
    Ring boot(broken);
    assert(boot.begin(0));
    broken.eraseLimit = cut;
    auto e = event();
    assert(boot.append(e) == AppendResult::Failed);
    broken.eraseLimit = -1;
    Ring reboot(broken);
    assert(reboot.begin(0));
    e = event();
    assert(reboot.append(e) == AppendResult::Persisted);
    assert(e.seq() == 8129);
  }
  Memory conflict = full;
  uint8_t header[64];
  std::memcpy(header, conflict.data.data(), 64);
  put32(header + 20, get32(header + 20) + 64);
  put32(header + 60, crc32(header, 60));
  std::memcpy(conflict.data.data() + 7 * 64, header, 64);
  Ring duplicate(conflict);
  assert(!duplicate.begin(0));
  conflict = full;
  uint8_t *old = conflict.data.data() + 7 * 64;
  put32(old + 12, 999);
  put32(old + 60, crc32(old, 60));
  Ring identity(conflict);
  assert(!identity.begin(0));
  // The legacy two-header layout remains a valid sparse journal.
  Memory legacy;
  {
    Ring first(legacy);
    assert(first.begin(88));
    auto e = event();
    assert(first.append(e) == AppendResult::Persisted);
  }
  assert(get32(legacy.data.data() + 8) == 1 &&
         get32(legacy.data.data() + 4096 + 8) == 2);
  Ring compatible(legacy);
  assert(compatible.begin(0) && compatible.id() == 88);
}
void invalid() {
  Memory f;
  f.data[12345] = 1;
  auto before = f.data;
  Ring r(f);
  assert(!r.begin(1));
  assert(f.data == before);
  Memory shortF;
  shortF.data.resize(4096);
  Ring small(shortF);
  assert(!small.begin(1));
  Memory readFail;
  readFail.failRead = true;
  Ring fail(readFail);
  assert(!fail.begin(1));
  Memory duplicate;
  Ring dr(duplicate);
  assert(dr.begin(1));
  auto e = event();
  assert(dr.append(e) == AppendResult::Persisted);
  std::memcpy(duplicate.data.data() + Ring::Data + 64, e.bytes, 64);
  Ring dup(duplicate);
  assert(!dup.begin(1));
  Memory boundary;
  Ring br(boundary);
  assert(br.begin(1));
  for (size_t offset : {size_t(0), size_t(4096)}) {
    uint8_t *p = boundary.data.data() + offset;
    if (std::memcmp(p, "BBR1", 4))
      continue;
    put32(p + 20, UINT32_MAX);
    put32(p + 60, crc32(p, 60));
  }
  Ring end(boundary);
  assert(end.begin(0));
  assert(end.append(e) == AppendResult::Unavailable);
}
// Empty SPIFFS formatted over the ring range: only per-block magic remains.
void formatSpiffs(Memory &f, uint32_t blockCount) {
  for (size_t s = 0; s < f.data.size() / Ring::Sector; ++s) {
    const uint16_t magic = uint16_t(0x20140529u ^ 256u ^ (blockCount - s));
    f.data[s * Ring::Sector + 252] = uint8_t(magic);
    f.data[s * Ring::Sector + 253] = uint8_t(magic >> 8);
  }
}
void emptySpiffs() {
  Memory f;
  formatSpiffs(f, 48);
  // Same bytes a field TTGO held after the older 192 KiB min_spiffs layout.
  assert(f.data[252] == 0x19 && f.data[253] == 0x04);
  assert(f.data[Ring::Sector + 252] == 0x06);
  auto before = f.data;
  Ring retry(f);
  assert(!retry.begin(0)); // periodic retries without a new identity never erase
  assert(f.data == before && f.erases == 0);
  Ring r(f);
  assert(r.begin(0x77));
  assert(f.erases == Ring::Size / Ring::Sector);
  auto e = event();
  assert(r.append(e) == AppendResult::Persisted);
  Ring reboot(f);
  assert(reboot.begin(0) && reboot.id() == 0x77);

  Memory interrupted; // power cut after the first sectors were erased
  formatSpiffs(interrupted, 48);
  std::fill(interrupted.data.begin(), interrupted.data.begin() + 5 * Ring::Sector, 255);
  Ring resumed(interrupted);
  assert(resumed.begin(1));
  Memory counted; // erase count stored after the magic in every block
  formatSpiffs(counted, 32);
  for (size_t s = 0; s < Ring::Size / Ring::Sector; ++s) {
    counted.data[s * Ring::Sector + 254] = 0;
    counted.data[s * Ring::Sector + 255] = 0;
  }
  Ring withCount(counted);
  assert(withCount.begin(1));

  const size_t lookupEntry = 2 * Ring::Sector, secondPage = 7 * Ring::Sector + 256 + 252;
  for (int mutation = 0; mutation < 4; ++mutation) {
    Memory used;
    formatSpiffs(used, 48);
    if (mutation == 0)
      used.data[lookupEntry] = 1; // a file's lookup entry
    if (mutation == 1)
      used.data[secondPage] = 0; // data outside the lookup tail
    if (mutation == 2) {
      const uint16_t other = uint16_t(0x20140529u ^ 256u ^ (47u - 5u));
      used.data[5 * Ring::Sector + 252] = uint8_t(other);
      used.data[5 * Ring::Sector + 253] = uint8_t(other >> 8);
    }
    if (mutation == 3) {
      used.data[3 * Ring::Sector + 252] = 255; // erase count without magic
      used.data[3 * Ring::Sector + 253] = 255;
      used.data[3 * Ring::Sector + 254] = 0;
    }
    auto snapshot = used.data;
    Ring rejected(used);
    assert(!rejected.begin(1));
    assert(used.data == snapshot && used.erases == 0);
  }
  Memory failing;
  formatSpiffs(failing, 48);
  failing.eraseLimit = 0;
  auto unchanged = failing.data;
  Ring broken(failing);
  assert(!broken.begin(1));
  assert(failing.data == unchanged);
  assert(broken.status() == RingStatus::EraseError);
}
// Arduino 2.x min_spiffs: 192 KiB spiffs. The ring uses its first 128 KiB.
constexpr size_t Legacy = 3 * 65536;
void legacyPartition() {
  Memory blank;
  blank.data.assign(Legacy, 255);
  Ring r(blank);
  assert(r.begin(0x31) && r.status() == RingStatus::Ready);
  auto e = event();
  assert(r.append(e) == AppendResult::Persisted);
  assert(std::all_of(blank.data.begin() + Ring::Size, blank.data.end(),
                     [](uint8_t b) { return b == 255; }));
  Ring reboot(blank);
  assert(reboot.begin(0) && reboot.id() == 0x31);

  Memory formatted; // the field layout: empty SPIFFS over all 48 blocks
  formatted.data.assign(Legacy, 255);
  formatSpiffs(formatted, 48);
  auto before = formatted.data;
  Ring retry(formatted);
  assert(!retry.begin(0) && retry.status() == RingStatus::Unopened);
  assert(formatted.data == before && formatted.erases == 0);
  Ring adopted(formatted);
  assert(adopted.begin(0x32) && adopted.status() == RingStatus::Ready);
  assert(formatted.erases == Legacy / Ring::Sector);
  assert(std::all_of(formatted.data.begin() + Ring::Size, formatted.data.end(),
                     [](uint8_t b) { return b == 255; }));

  Memory file; // a file lookup entry beyond the ring keeps everything intact
  file.data.assign(Legacy, 255);
  formatSpiffs(file, 48);
  file.data[40 * Ring::Sector] = 1;
  auto kept = file.data;
  Ring refused(file);
  assert(!refused.begin(1) && refused.status() == RingStatus::ForeignData);
  assert(file.data == kept && file.erases == 0);

  Memory ragged;
  ragged.data.assign(Ring::Size + 100, 255);
  Ring odd(ragged);
  assert(!odd.begin(1) && odd.status() == RingStatus::BadSize);
  Memory large; // e.g. the 1.4 MiB default-scheme spiffs: not adopted
  large.data.assign(Ring::MaxSize + Ring::Sector, 255);
  Ring big(large);
  assert(!big.begin(1) && big.status() == RingStatus::BadSize && large.erases == 0);
}
void statusCodes() {
  Memory fresh;
  Ring unopened(fresh);
  assert(unopened.status() == RingStatus::Unopened);
  Memory foreign;
  foreign.data[12345] = 1;
  Ring r(foreign);
  assert(!r.begin(1) && r.status() == RingStatus::ForeignData);
  Memory small;
  small.data.resize(4096);
  Ring s(small);
  assert(!s.begin(1) && s.status() == RingStatus::BadSize);
  Memory unreadable;
  unreadable.failRead = true;
  Ring u(unreadable);
  assert(!u.begin(1) && u.status() == RingStatus::ReadError);
  Memory duplicate;
  Ring d(duplicate);
  assert(d.begin(1));
  auto e = event();
  assert(d.append(e) == AppendResult::Persisted);
  std::memcpy(duplicate.data.data() + Ring::Data + 64, e.bytes, 64);
  Ring dup(duplicate);
  assert(!dup.begin(1) && dup.status() == RingStatus::RecordConflict);
  // Console values are documented; keep them stable.
  assert(uint8_t(RingStatus::Ready) == 1 && uint8_t(RingStatus::ForeignData) == 5 &&
         uint8_t(RingStatus::RecordConflict) == 8);
}
void protocol() {
  Batch b;
  b.ring = 0x1234;
  b.token = 0xdeadbeef;
  b.count = 2;
  b.records[0] = event();
  b.records[0].sequence(1);
  b.records[1] = event();
  b.records[1].sequence(65);
  char payload[4096];
  auto n = encodeBatch(b, "iotglove", "AABBCCDDEEFF", payload, sizeof(payload));
  assert(n > 256 && n < 4096);
  assert(std::strncmp(payload,
                      "BB1 iotglove AABBCCDDEEFF 0000000000001234 deadbeef\n",
                      51) == 0);
  const char *ack = "ACK1 0000000000001234 deadbeef 65\n";
  assert(exactAck(ack, std::strlen(ack), b));
  assert(!exactAck(ack, std::strlen(ack) - 1, b));
  assert(!exactAck("ACK1 0000000000001234 deadbeef 64\n", 33, b));
  ++b.token;
  assert(!exactAck(ack, std::strlen(ack), b));
  assert(
      !encodeBatch(b, "bad device", "AABBCCDDEEFF", payload, sizeof(payload)));
  assert(!encodeBatch(b, "iotglove", "aabbccddeeff", payload, sizeof(payload)));
  assert(!encodeBatch(b, "iotglove", "AABBCCDDEEFF", payload, 100));
  b.count = 16;
  for (size_t i = 0; i < 16; ++i) {
    b.records[i] = event();
    b.records[i].sequence(uint32_t(i + 1));
  }
  assert(encodeBatch(b, "HAS1_generator", "AABBCCDDEEFF", payload,
                     sizeof(payload)) < 4096);
  assert(encodeBatch(b, "HAS1_altar", "AABBCCDDEEFF", payload,
                     sizeof(payload)) < 4096);
  assert(!encodeBatch(b, "HAS1_duct", "AABBCCDDEEFF", payload,
                      sizeof(payload)));
  b.count = 17;
  assert(!encodeBatch(b, "iotglove", "AABBCCDDEEFF", payload, sizeof(payload)));
  assert(backoffMs(0) == 5000 && backoffMs(4) == 60000);
}
void evidence() {
  const uint8_t address[]={172,30,1,43};
  assert(ipv4FromNetworkBytes(address)==0xac1e012b);
  EvidenceGate g;
  assert(!g.observe(0,false,0,5,200));
  assert(g.observe(10,true,1,1,0));
  assert(!g.observe(100,true,1,1,0));
  assert(g.observe(1010,true,2,2,0));
  assert(g.observe(2010,false,0,5,200));
  assert(!g.observe(3010,false,0,5,200));
  assert(g.observe(UINT32_MAX-10,true,1,1,0));
  assert(g.observe(1000,true,2,2,0));
  EvidenceGate pending;
  assert(pending.observe(100,true,1,1,0));
  assert(!pending.observe(200,false,0,5,200));
  assert(!pending.observe(300,false,0,5,200));
  assert(pending.observe(1100,false,0,5,200));
  assert(!pending.observe(1200,false,0,5,200));
  ClockCoverage c;
  c.observe(10,20,100,0);c.observe(10,20,105,0);
  assert(c.first==100 && c.last==105);
  c.observe(10,20,106,1);
  assert(c.first==106 && c.last==106 && c.wraps==1);
  c.observe(10,21,200,0);
  assert(c.boot==21 && c.first==200 && c.wraps==0);
  Record old=Record::make(EventType::Recovery,20,500);old.sequence(201);
  c.observeCurrentRecord(10,21,old,0);
  assert(c.boot==21 && c.first==200 && c.last==200);
  Record current=Record::make(EventType::Recovery,21,1000);current.sequence(202);
  c.observeCurrentRecord(10,21,current,0);
  assert(c.boot==21 && c.first==200 && c.last==202);
  char token[33];
  const char *probe="T1 0123456789abcdef0123456789abcdef\n";
  assert(parseTimeProbe(probe,36,token));
  assert(!parseTimeProbe(probe,35,token));
  assert(!parseTimeProbe("T1 0123456789abcdef0123456789abcdeF\n",36,token));
  char response[320];
  assert(parseTimeProbe(probe,36,token));
  assert(encodeTimeProbe(c,"AABBCCDDEEFF",token,1000,response,sizeof(response)));
  assert(std::strstr(response,"\"boot\":21"));
  assert(std::strstr(response,"\"seq_min\":200"));
  assert(!encodeTimeProbe(c,"AABBCCDDEEFF",token,1ull<<32,response,sizeof(response)));
  assert(!encodeTimeProbe(c,"AABBCCDDEEFF",token,1000,response,20));
  RetainedEvidence rtc{};rtc.clear();
  Record recovered[RetainedEvidence::Capacity];
  for(unsigned i=0;i<10;++i)rtc.add(Record::make(EventType::Recovery,100+i,200+i,i,0,0,0,"fault"));
  assert(rtc.recover(recovered,false)==0);
  assert(rtc.recover(recovered,true)==8);
  assert(get32(recovered[0].bytes+8)==102 && get32(recovered[7].bytes+8)==109);
  assert(get32(recovered[0].bytes+12)==202 && get32(recovered[7].bytes+12)==209);
  rtc.entries[1][16]^=1; // torn/corrupted latest payload cannot be replayed.
  assert(rtc.recover(recovered,true)==7);
  rtc.magic=0;
  assert(rtc.recover(recovered,true)==0);
}
void mqttIdentity() {
  const char *mac="AABBCCDDEEFF";
  assert(std::strcmp(resolveMqttUsername("","mqtt://127.0.0.1:1883",mac),mac)==0);
  assert(std::strcmp(resolveMqttUsername(nullptr,"mqtt://127.0.0.1:1883",mac),mac)==0);
  assert(std::strcmp(resolveMqttUsername("fixture-user","mqtt://127.0.0.1:1883",mac),"fixture-user")==0);
  assert(std::strcmp(resolveMqttUsername("fixture-user","",mac),"fixture-user")==0);
  assert(!resolveMqttUsername("","",mac)[0]);
  assert(!resolveMqttUsername(nullptr,nullptr,mac)[0]);
  assert(!resolveMqttUsername(nullptr,"mqtt://127.0.0.1:1883",nullptr)[0]);
}
// Opt-in large partition: the default 4 MB scheme's 1.4 MiB spiffs
// (0x170000 = 368 sectors) on a tag machine. Records every offset touched.
constexpr size_t Default4M = 0x170000;
struct Tracked : Memory {
  size_t highestRead = 0, highestWrite = 0, highestErase = 0;
  unsigned readsBeyond = 0;
  Tracked() { data.assign(Default4M, 255); }
  bool read(size_t o, uint8_t *b, size_t n) override {
    highestRead = std::max(highestRead, o + n);
    if (o + n > Ring::Size)
      ++readsBeyond;
    return Memory::read(o, b, n);
  }
  bool write(size_t o, const uint8_t *b, size_t n) override {
    highestWrite = std::max(highestWrite, o + n);
    return Memory::write(o, b, n);
  }
  bool erase(size_t o, size_t n) override {
    highestErase = std::max(highestErase, o + n);
    return Memory::erase(o, n);
  }
  void resetCounters() {
    highestRead = highestWrite = highestErase = 0;
    readsBeyond = 0;
    writes = erases = 0;
  }
};
bool sameBeyondRing(const Memory &f, const std::vector<uint8_t> &before) {
  return f.data.size() == before.size() &&
         std::equal(f.data.begin() + Ring::Size, f.data.end(),
                    before.begin() + Ring::Size);
}
void largePartition() {
  static_assert(Ring::LargeMaxSize == 0x200000, "2 MiB cap");
  static_assert(Ring::maxSize(false) == Ring::MaxSize, "default unchanged");
  // Off (the default): exactly the previous size gate.
  for (bool explicitOff : {false, true}) {
    Tracked off;
    Ring r(off);
    assert(!(explicitOff ? r.begin(1, false) : r.begin(1)));
    assert(r.status() == RingStatus::BadSize);
    assert(off.erases == 0 && off.writes == 0);
  }
  // On, blank: ready without any erase; nothing beyond the ring is written or
  // erased across appends, ACKed recycling and unACKed (LOST) recycling.
  Tracked blank;
  Ring r(blank);
  assert(r.begin(0x51, true) && r.status() == RingStatus::Ready);
  assert(blank.erases == 0 && blank.highestRead == Default4M);
  Record e;
  for (size_t i = 0; i < Ring::Slots; ++i) {
    e = event();
    assert(r.append(e) == AppendResult::Persisted);
  }
  assert(r.acknowledge(64));
  e = event();
  assert(r.append(e) == AppendResult::Persisted); // ACKed sector recycled
  for (size_t i = 0; i < 2 * Ring::Slots; ++i) {
    e = event();
    assert(r.append(e) == AppendResult::Persisted);
  }
  assert(r.lostRecords() > 0); // oldest-sector overwrite happened
  assert(blank.erases > 0);
  assert(blank.highestWrite <= Ring::Size && blank.highestErase <= Ring::Size);
  assert(std::all_of(blank.data.begin() + Ring::Size, blank.data.end(),
                     [](uint8_t b) { return b == 255; }));
  // Reopen: metadata is found; nothing beyond the ring is even read.
  blank.resetCounters();
  Ring reboot(blank);
  assert(reboot.begin(0, true) && reboot.id() == 0x51);
  assert(blank.readsBeyond == 0 && blank.highestRead <= Ring::Size);
  assert(blank.writes == 0 && blank.erases == 0);
  // The same ring opened without the option is refused, not reinitialized.
  Ring withoutOption(blank);
  assert(!withoutOption.begin(1) &&
         withoutOption.status() == RingStatus::BadSize);
  assert(blank.writes == 0 && blank.erases == 0);

  // On, empty SPIFFS over all 368 blocks: only the 32 ring sectors are
  // erased; the magics beyond the ring stay byte-for-byte.
  Tracked formatted;
  formatSpiffs(formatted, Default4M / Ring::Sector);
  auto before = formatted.data;
  Ring retry(formatted);
  assert(!retry.begin(0, true) && retry.status() == RingStatus::Unopened);
  assert(formatted.data == before && formatted.erases == 0);
  Ring adopted(formatted);
  assert(adopted.begin(0x52, true) && adopted.status() == RingStatus::Ready);
  assert(formatted.erases == Ring::Size / Ring::Sector);
  assert(formatted.highestErase == Ring::Size &&
         formatted.highestWrite <= Ring::Size);
  assert(sameBeyondRing(formatted, before));
  e = event();
  assert(adopted.append(e) == AppendResult::Persisted);
  formatted.resetCounters();
  Ring reopened(formatted);
  assert(reopened.begin(0, true) && reopened.id() == 0x52);
  assert(formatted.readsBeyond == 0 && formatted.erases == 0);
  // With erase counts stored after each magic, as SPIFFS also writes.
  Tracked counted;
  formatSpiffs(counted, Default4M / Ring::Sector);
  for (size_t s = 0; s < Default4M / Ring::Sector; ++s)
    counted.data[s * Ring::Sector + 254] = counted.data[s * Ring::Sector + 255] = 0;
  before = counted.data;
  Ring withCount(counted);
  assert(withCount.begin(0x53, true));
  assert(counted.erases == Ring::Size / Ring::Sector);
  assert(sameBeyondRing(counted, before));

  // A file-like byte inside the ring region or anywhere beyond it, on a blank
  // or a formatted partition: ForeignData with zero erases and writes.
  for (bool spiffs : {false, true})
    for (size_t offset : {size_t(Ring::Data + 100), size_t(Ring::Size - 1),
                          size_t(Ring::Size), size_t(200 * Ring::Sector + 17),
                          size_t(Default4M - 1)}) {
      Tracked foreign;
      if (spiffs)
        formatSpiffs(foreign, Default4M / Ring::Sector);
      foreign.data[offset] = 0x42;
      auto kept = foreign.data;
      Ring refused(foreign);
      assert(!refused.begin(1, true) &&
             refused.status() == RingStatus::ForeignData);
      assert(foreign.data == kept && foreign.erases == 0 && foreign.writes == 0);
    }
  // An inconsistent magic far beyond the ring is foreign too.
  Tracked mixed;
  formatSpiffs(mixed, Default4M / Ring::Sector);
  const uint16_t other = uint16_t(0x20140529u ^ 256u ^ (300u - 250u));
  mixed.data[250 * Ring::Sector + 252] = uint8_t(other);
  mixed.data[250 * Ring::Sector + 253] = uint8_t(other >> 8);
  auto mixedBefore = mixed.data;
  Ring inconsistent(mixed);
  assert(!inconsistent.begin(1, true) &&
         inconsistent.status() == RingStatus::ForeignData);
  assert(mixed.data == mixedBefore && mixed.erases == 0);

  // Power cut during the one-time ring erase (after N whole sectors, or part
  // way through the next sector before its magic was cleared) or after it,
  // before the first checkpoint: the next boot still sees an unused partition
  // and finishes.
  for (size_t sectors : {size_t(0), size_t(1), size_t(5), size_t(31), size_t(32)})
    for (int partial : {-1, 0, 128}) {
      if (sectors == 32 && partial >= 0)
        continue;
      Tracked cut;
      formatSpiffs(cut, Default4M / Ring::Sector);
      before = cut.data;
      std::fill(cut.data.begin(), cut.data.begin() + sectors * Ring::Sector, 255);
      if (partial >= 0)
        std::fill(cut.data.begin() + sectors * Ring::Sector,
                  cut.data.begin() + sectors * Ring::Sector + partial, 255);
      Ring resumed(cut);
      assert(resumed.begin(0x60 + sectors, true));
      assert(resumed.status() == RingStatus::Ready);
      assert(cut.erases == (sectors == 32 ? 0u : Ring::Size / Ring::Sector));
      assert(sameBeyondRing(cut, before));
      e = event();
      assert(resumed.append(e) == AppendResult::Persisted);
      Ring again(cut);
      assert(again.begin(0, true) && again.id() == 0x60 + sectors);
    }
  // An erase fault during the ring erase leaves the rest untouched and the
  // partition still adoptable on a later boot.
  for (int limit : {0, 1024}) {
    Tracked faulty;
    formatSpiffs(faulty, Default4M / Ring::Sector);
    before = faulty.data;
    faulty.eraseLimit = limit;
    Ring broken(faulty);
    assert(!broken.begin(1, true) && broken.status() == RingStatus::EraseError);
    assert(sameBeyondRing(faulty, before) && faulty.writes == 0);
    faulty.eraseLimit = -1;
    Ring later(faulty);
    assert(later.begin(2, true) && sameBeyondRing(faulty, before));
  }

  // Size gate with the option on.
  for (size_t bytes : {Default4M + 100, size_t(Ring::LargeMaxSize + Ring::Sector),
                       size_t(Ring::Size - Ring::Sector)}) {
    Tracked odd;
    odd.data.assign(bytes, 255);
    Ring sized(odd);
    assert(!sized.begin(1, true) && sized.status() == RingStatus::BadSize);
    assert(odd.erases == 0 && odd.writes == 0);
  }
  Tracked cap;
  cap.data.assign(Ring::LargeMaxSize, 255);
  Ring atCap(cap);
  assert(atCap.begin(1, true) && cap.highestWrite <= Ring::Size);
  // Up to MaxSize the option changes nothing: a formatted 192 KiB partition
  // is still erased and verified whole.
  Tracked legacy;
  legacy.data.assign(Legacy, 255);
  formatSpiffs(legacy, 48);
  Ring legacyOn(legacy);
  assert(legacyOn.begin(3, true));
  assert(legacy.erases == Legacy / Ring::Sector);
  assert(std::all_of(legacy.data.begin() + Ring::Size, legacy.data.end(),
                     [](uint8_t b) { return b == 255; }));
}
void tagmachineToken() {
  assert(knownDevice("HAS3_tagmachine") && knownDevice("iotglove") &&
         knownDevice("HAS1_generator") && knownDevice("HAS1_altar"));
  assert(!knownDevice(nullptr) && !knownDevice("") &&
         !knownDevice("HAS3_TAGMACHINE") && !knownDevice("tagmachine") &&
         !knownDevice("HAS3_tagmachine "));
  Batch b;
  b.ring = 0x1234;
  b.token = 0xdeadbeef;
  b.count = 1;
  b.records[0] = event();
  b.records[0].sequence(7);
  char payload[4096];
  auto n = encodeBatch(b, "HAS3_tagmachine", "2CBCBBA7C18C", payload,
                       sizeof(payload));
  assert(n > 128 && n < 4096);
  const char *head = "BB1 HAS3_tagmachine 2CBCBBA7C18C 0000000000001234 deadbeef\n";
  assert(std::strncmp(payload, head, std::strlen(head)) == 0);
  assert(!encodeBatch(b, "HAS3_unknown", "2CBCBBA7C18C", payload,
                      sizeof(payload)));
}
int main() {
  mqttIdentity();
  evidence();
  records();
  recovery();
  fullAndRecycle();
  tornRecords();
  tornMetadata();
  partialErase();
  rollingPowerCuts();
  metadataJournal();
  invalid();
  emptySpiffs();
  legacyPartition();
  statusCodes();
  protocol();
  largePartition();
  tagmachineToken();
  std::cout << "blackbox core: all fault and protocol tests passed\n";
}
