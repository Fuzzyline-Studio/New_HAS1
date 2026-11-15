#pragma once
#include <stdint.h>
#include "network_recovery.h"

namespace iotglove {
// Retained across our requested SW restarts and watchdog resets. No flash writes.
// GWR3 invalidates the former three-attempt budget semantics after an upgrade.
// `attempts` is a diagnostic count only: it never delays a later restart.
struct NetworkRestartHistory {
  uint32_t magic;
  uint32_t attempts;
  bool requested;
};
struct NetworkRestartDecision {
  uint8_t cause = 0;  // Same recovery cause contract: 2 worker stall, 3 Wi-Fi down.
  bool restored = false, restart = false;
  uint32_t previousAttempts = 0;
};
class NetworkRestartPolicy {
 public:
  static constexpr uint32_t kMagic = 0x47575233U;
  static constexpr uint32_t kStableMs = 10U * 60U * 1000U;
  static constexpr uint32_t kMaxProgressGapMs = 180000U;
  static void boot(NetworkRestartHistory& history, bool softwareReset, bool watchdogReset) {
    if (history.magic != kMagic || (!softwareReset && !watchdogReset) ||
        (softwareReset && !history.requested)) {
      history = {kMagic, 0, false}; return;
    }
    if (watchdogReset && !history.requested && history.attempts != UINT32_MAX)
      ++history.attempts;
    history.requested = false;
  }
  static uint8_t diagnosticAttempt(uint32_t attempts) {
    return attempts > UINT8_MAX ? UINT8_MAX : static_cast<uint8_t>(attempts);
  }
  // Testable production monitor. A fault is due the moment its own limit is
  // reached (worker 20/30s, link 30s, never associated 180s), however many
  // restarts came before: a boot that has not associated yet already waits
  // 180s, which is what paces restarts while the AP is really off.
  NetworkRestartDecision poll(NetworkRestartHistory& history, const NetworkDiagnostics& d,
                              uint32_t now, bool otaBusy = false) {
    NetworkRestartDecision out;
    const bool ota = otaBusy || d.phase == NetworkPhase::Ota;
    const bool stalled = !ota && networkWorkerStalled(d, now);
    if (observeHealthy(now, d.sequence, !ota && d.wifiConnected && !stalled) && history.attempts) {
      out.restored = true; out.previousAttempts = history.attempts;
      history = {kMagic, 0, false};
    }
    const bool linkDown = !ota && wifiLinkDownTooLong(d, now);
    out.cause = stalled ? 2 : linkDown ? 3 : 0;
    if (!out.cause) return out;
    if (history.attempts != UINT32_MAX) ++history.attempts;
    history.requested = true; out.restart = true;
    return out;
  }
 private:
  bool tracking_ = false;
  uint32_t since_ = 0, advancedAt_ = 0, sequence_ = 0;
  bool observeHealthy(uint32_t now, uint32_t sequence, bool linkAndWorkerHealthy) {
    if (!linkAndWorkerHealthy) { tracking_ = false; return false; }
    if (!tracking_) {
      tracking_ = true; since_ = advancedAt_ = now; sequence_ = sequence; return false;
    }
    if (sequence != sequence_) { sequence_ = sequence; advancedAt_ = now; }
    if (uint32_t(now-advancedAt_) > kMaxProgressGapMs) { tracking_ = false; return false; }
    if (uint32_t(now-since_) < kStableMs) return false;
    since_ = now; return true;
  }
};
}  // namespace iotglove
