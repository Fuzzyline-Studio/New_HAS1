#include "network_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;
int main() {
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.role = Role::Player;
  s.receivedAtMs = 100;
  assert(serverSnapshotFresh(s, 100 + kServerFreshMs - 1));
  assert(!serverSnapshotFresh(s, 100 + kServerFreshMs));
  s.valid = false; assert(!serverSnapshotFresh(s, 100));
  s.valid = true; s.receivedAtMs = UINT32_MAX - 100U;
  assert(serverSnapshotFresh(s, kServerFreshMs - 102U));
  assert(!serverSnapshotFresh(s, kServerFreshMs - 101U));
  strcpy(s.deviceName, "G1P3"); strcpy(s.session, "boot-1");
  GameEvent e;
  strcpy(e.deviceName, s.deviceName); strcpy(e.session, s.session);
  e.kind = GameEvent::Kind::SetCount; e.value = 1;
  assert(!commandAllowed(e, s));
  s.role = Role::Tagger; assert(!commandAllowed(e, s));
  s.role = Role::Neutral; assert(!commandAllowed(e, s));
  s.role = Role::Ghost;
  assert(commandAllowed(e, s)); assert(!commandApplied(e, s));
  s.revivalCount = 1; assert(commandApplied(e, s));
  e.value = 3; assert(commandAllowed(e, s));
  e.value = 4; assert(!commandAllowed(e, s));
  s.sacrificed = true; assert(commandAllowed(e, s));
  e.value = 5; assert(!commandAllowed(e, s));
  e.value = 0; assert(commandAllowed(e, s));
  s.phase = Phase::Ready; assert(!commandAllowed(e, s));
  s.phase = Phase::Active; s.deviceState = DeviceState::Setting;
  assert(!commandAllowed(e, s));
  s.deviceState = DeviceState::Other; s.valid = false;
  assert(!commandAllowed(e, s)); s.valid = true;
  strcpy(s.session, "boot-2");
  assert(!commandAllowed(e, s)); assert(!commandApplied(e, s));
  strcpy(s.session, e.session); strcpy(s.deviceName, "G1P4");
  assert(!commandAllowed(e, s));
  strcpy(s.deviceName, e.deviceName);
  e.kind = static_cast<GameEvent::Kind>(255); assert(!commandAllowed(e, s));
  // Loop is the periodic request; ReceiveMine only without a snapshot, when forced,
  // flagged, or after 5 s for every role, including Ghost. The server flags each
  // automatic charge now, so the former Ghost-only 1 s refresh must not return.
  assert(fullRefreshDue(false, false, false, 100, 1100));
  assert(fullRefreshDue(true, true, false, 100, 1100));
  assert(fullRefreshDue(true, false, true, 100, 1100));
  assert(!fullRefreshDue(true, false, false, 100, 1100));
  assert(!fullRefreshDue(true, false, false, 100, 5099));
  assert(fullRefreshDue(true, false, false, 100, 5100));
  assert(!fullRefreshDue(true, false, false, UINT32_MAX - 99U, 900U));
  assert(!fullRefreshDue(true, false, false, UINT32_MAX - 99U, 4899U));
  assert(fullRefreshDue(true, false, false, UINT32_MAX - 99U, 4900U));
  assert(fullRefreshDue(false, false, false, UINT32_MAX - 99U, 900U));
  assert(fullRefreshDue(true, true, false, UINT32_MAX - 99U, 900U));
  assert(fullRefreshDue(true, false, true, UINT32_MAX - 99U, 900U));
  // Bounded wrap-safe deadlines for the post-button quiet window and event gate.
  assert(!pendingUntil(0, 0, 1000) && pendingUntil(1, 0, 1000) && pendingUntil(1000, 0, 1000));
  assert(!pendingUntil(1001, 0, 1000) && !pendingUntil(500, 501, 1000));
  assert(pendingUntil(299, UINT32_MAX - 100U, 1000) && !pendingUntil(299, 299, 1000));
  assert(!pendingUntil(0, UINT32_MAX / 2U, 1000) && !pendingUntil(0, UINT32_MAX / 2U + 2U, 1000));
  e.notBeforeMs = 400;
  assert(!eventDue(e, 0) && !eventDue(e, 399) && eventDue(e, 400) && eventDue(e, 400 + UINT32_MAX / 2U));
  puts("PASS: count-only commands, authoritative ghost role and session/freshness gates");
}
