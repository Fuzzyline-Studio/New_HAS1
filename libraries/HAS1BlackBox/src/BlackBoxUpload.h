#pragma once
#include "BlackBoxRecord.h"
#include <cstdio>
namespace has1blackbox {
constexpr size_t BatchRecords = 16, PayloadCapacity = 4096;
struct Batch {
  uint64_t ring = 0;
  uint32_t token = 0;
  uint32_t generation = 0;
  size_t count = 0;
  Record records[BatchRecords]{};
};
// Device tokens the collector accepts in a batch header.
inline bool knownDevice(const char *device) {
  return device && (!std::strcmp(device, "iotglove") ||
                    !std::strcmp(device, "HAS1_generator") ||
                    !std::strcmp(device, "HAS1_altar") ||
                    !std::strcmp(device, "HAS3_tagmachine"));
}
inline size_t encodeBatch(const Batch &batch, const char *device,
                          const char *mac, char *out, size_t cap) {
  if (!batch.count || batch.count > BatchRecords || !device || !mac ||
      std::strlen(mac) != 12)
    return 0;
  if (!knownDevice(device))
    return 0;
  for (size_t i = 0; i < 12; ++i)
    if (!((mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'A' && mac[i] <= 'F')))
      return 0;
  int n =
      std::snprintf(out, cap, "BB1 %s %s %016llx %08lx\n", device, mac,
                    (unsigned long long)batch.ring, (unsigned long)batch.token);
  if (n < 0 || size_t(n) >= cap)
    return 0;
  size_t used = size_t(n);
  uint32_t last = 0;
  for (size_t i = 0; i < batch.count; ++i) {
    const Record &r = batch.records[i];
    if (!r.valid() || r.seq() <= last || used + 130 > cap)
      return 0;
    last = r.seq();
    r.hex(out + used, cap - used);
    used += 128;
    out[used++] = '\n';
    out[used] = 0;
  }
  return used;
}
inline bool exactAck(const char *payload, size_t len, const Batch &b) {
  if (!b.count)
    return false;
  char expected[80];
  int n = std::snprintf(expected, sizeof(expected), "ACK1 %016llx %08lx %lu\n",
                        (unsigned long long)b.ring, (unsigned long)b.token,
                        (unsigned long)b.records[b.count - 1].seq());
  return n > 0 && len == size_t(n) && !std::memcmp(payload, expected, len);
}
inline uint32_t backoffMs(unsigned failures) {
  return failures >= 4 ? 60000u : (5000u << failures);
}
} // namespace has1blackbox
