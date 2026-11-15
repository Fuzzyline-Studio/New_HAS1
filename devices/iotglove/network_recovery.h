#pragma once
#include <stdint.h>

namespace iotglove {
enum class NetworkPhase : uint8_t { Starting, Idle, Connecting, Backoff, Http, Ota };
struct NetworkDiagnostics {
  NetworkPhase phase = NetworkPhase::Starting;
  uint32_t sequence = 0, startedAtMs = 0, progressAtMs = 0, completedAtMs = 0;
  uint32_t deadlineMs = 0, idleDeadlineMs = 0;
  uint32_t lastHttpElapsedMs = 0, lastHttpBytes = 0;
  uint32_t primaryTransportFailures = 0, forcedReconnects = 0, reconnectCooldownMs = 0;
  uint32_t workerStackHighWater = 0, heapFree = 0, heapLargest = 0;
  int lastHttpStatus = 0;
  uint8_t lastHttpError = 0, lastHttpPhase = 0;
  uint16_t wifiDisconnectReason = 0;
  // STA link itself, distinct from HTTP. The worker owns every field.
  bool wifiConnected = false;        // associated to the fixed SSID
  bool wifiWasUp = false;            // associated at least once since boot
  uint32_t wifiDownSinceMs = 0;      // start of the current outage; boot time until first association
  uint32_t wifiConnectFailures = 0;  // consecutive failed fixed-SSID attempts in this outage
  uint32_t wifiLossCount = 0;        // disconnects of an established link since boot
  uint32_t wifiLastLossAtMs = 0;     // uptime of the latest such disconnect event
  uint16_t wifiLastLossReason = 0;   // its STA disconnect reason
  int8_t wifiLastLossRssi = 0;       // RSSI sampled shortly before it
  int8_t rssi = 0;                   // latest sample while connected
};

// A STA that cannot reassociate is restarted as a last resort: after some drops
// the radio/driver only recovers through a reboot, and the retry loop alone
// never ends. A boot that has not associated yet keeps the longer limit, since
// the AP may simply be off. OTA keeps the exemption of the worker watchdog.
constexpr uint32_t kWifiLostRestartMs = 30000;
constexpr uint32_t kWifiNeverUpRestartMs = 180000;

inline bool wifiLinkDownTooLong(const NetworkDiagnostics& d, uint32_t now) {
  if (d.wifiConnected || d.phase == NetworkPhase::Ota) return false;
  const uint32_t limit = d.wifiWasUp ? kWifiLostRestartMs : kWifiNeverUpRestartMs;
  return uint32_t(now - d.wifiDownSinceMs) >= limit;
}

// A fixed-SSID attempt ends as soon as the driver reports failure, so this
// pause alone spaces attempts. After a power cut the AP keeps the lost session
// and can stay out of scans for tens of seconds: retry quickly first, then
// back off for a real outage. `failures` counts this outage's failed attempts.
inline uint32_t wifiConnectRetryMs(uint32_t failures) { return failures <= 3 ? 1000 : 5000; }

inline bool networkWorkerStalled(const NetworkDiagnostics& d, uint32_t now) {
  // Legacy OTA owns its timeouts. Without a progress hook, the worker watchdog
  // cannot distinguish a healthy flash/download from a stalled OTA operation.
  if (d.phase == NetworkPhase::Ota) return false;
  return (d.deadlineMs && uint32_t(now - d.startedAtMs) >= d.deadlineMs) ||
      (d.idleDeadlineMs && uint32_t(now - d.progressAtMs) >= d.idleDeadlineMs);
}

// Owned by the network worker. Only its periodic poll observation (Loop, then ReceiveMine when read) is
// supplied here: side requests must neither inflate nor conceal poll failures.
class NetworkRecoveryPolicy {
 public:
  static constexpr uint32_t kFailureSpanMs = 15000;
  static constexpr uint32_t kHealthyResetMs = 60000;
  static constexpr uint32_t kMaxRetryAfterMs = 120000;

  void observePrimary(uint32_t now, bool transportFailure, bool validSnapshot) {
    if (transportFailure) {
      healthy_ = false;
      if (!failures_) firstFailureAt_ = now;
      if (failures_ != UINT32_MAX) ++failures_;
      return;
    }
    failures_ = 0;  // An HTTP error/invalid JSON still proves a responding path.
    if (!validSnapshot) { healthy_ = false; return; }
    if (!healthy_) { healthy_ = true; healthySince_ = now; }
    if (uint32_t(now - healthySince_) >= kHealthyResetMs) {
      recoveryLevel_ = 0;
      cooling_ = false;
    }
  }
  bool shouldReconnect(uint32_t now) const {
    return failures_ >= 3 && uint32_t(now - firstFailureAt_) >= kFailureSpanMs &&
        (!cooling_ || uint32_t(now - lastReconnectAt_) >= cooldownMs_);
  }
  void reconnected(uint32_t now, uint32_t entropy) {
    const uint32_t base = recoveryLevel_ == 0 ? 30000 : recoveryLevel_ == 1 ? 60000 : 120000;
    cooldownMs_ = base + entropy % 5001U;
    if (recoveryLevel_ < 2) ++recoveryLevel_;
    if (reconnects_ != UINT32_MAX) ++reconnects_;
    lastReconnectAt_ = now; cooling_ = true;
    failures_ = 0; healthy_ = false;
  }
  void disconnected() { failures_ = 0; healthy_ = false; }
  void defer(uint32_t now, uint32_t retryAfterMs) {
    if (!retryAfterMs) return;
    if (retryAfterMs > kMaxRetryAfterMs) retryAfterMs = kMaxRetryAfterMs;
    const uint32_t remaining = deferred(now) ? deferMs_ - uint32_t(now - deferAt_) : 0;
    if (retryAfterMs >= remaining) { deferAt_ = now; deferMs_ = retryAfterMs; }
  }
  bool deferred(uint32_t now) {
    if (deferMs_ && uint32_t(now - deferAt_) >= deferMs_) deferMs_ = 0;
    return deferMs_ != 0;
  }
  uint32_t failures() const { return failures_; }
  uint32_t reconnects() const { return reconnects_; }
  uint32_t cooldownMs() const { return cooling_ ? cooldownMs_ : 0; }
 private:
  uint32_t failures_ = 0, firstFailureAt_ = 0, reconnects_ = 0;
  uint32_t lastReconnectAt_ = 0, cooldownMs_ = 0, healthySince_ = 0;
  uint32_t deferAt_ = 0, deferMs_ = 0;
  uint8_t recoveryLevel_ = 0;
  bool cooling_ = false, healthy_ = false;
};
}  // namespace iotglove
