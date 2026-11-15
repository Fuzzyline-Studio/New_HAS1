#pragma once
#include "game_state.h"
#include "library_and_pin.h"
#include "state_policy.h"
#include <string.h>

namespace iotglove {
inline bool serverSnapshotFresh(const ServerSnapshot& snapshot, uint32_t now) {
  return snapshot.valid && uint32_t(now - snapshot.receivedAtMs) < kServerFreshMs;
}

inline bool sameEventSession(const GameEvent& e, const ServerSnapshot& s) {
  return s.valid && gameMutationsAllowed(s) &&
         strcmp(e.session, s.session) == 0 && strcmp(e.deviceName, s.deviceName) == 0;
}
inline bool commandAllowed(const GameEvent& e, const ServerSnapshot& s) {
  return sameEventSession(e, s) && e.kind == GameEvent::Kind::SetCount &&
         s.role == Role::Ghost && e.value <= (s.sacrificed ? 4 : 3);
}
inline bool commandApplied(const GameEvent& e, const ServerSnapshot& s) {
  return sameEventSession(e, s) && e.kind == GameEvent::Kind::SetCount &&
         s.role == Role::Ghost && s.revivalCount == e.value;
}

// Each poll cycle sends one Loop; a full ReceiveMine follows only when due. The
// server must flag automatic revival_count changes too. All roles share the same
// safety interval. `flagged` is any non-zero or unreadable shift_machine value.
constexpr uint32_t kFullRefreshMs = 5000;
inline bool fullRefreshDue(bool haveSnapshot, bool forced, bool flagged,
                           uint32_t lastRefreshMs, uint32_t now) {
  return !haveSnapshot || forced || flagged ||
      uint32_t(now - lastRefreshMs) >= kFullRefreshMs;
}

// Wrap-safe deadline: holds only while `until` is 1..maxMs ahead of now, so a
// past or never-set deadline cannot hold traffic back across millis() rollover.
constexpr uint32_t kNetworkQuietMaxMs = 1000;
inline bool pendingUntil(uint32_t until, uint32_t now, uint32_t maxMs) {
  return uint32_t(until - now) - 1U < maxMs;
}
inline bool eventDue(const GameEvent& e, uint32_t now) {
  return !pendingUntil(e.notBeforeMs, now, kNetworkQuietMaxMs);
}
}  // namespace iotglove
