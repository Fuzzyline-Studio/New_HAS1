#include "network_restart.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using namespace iotglove;
static NetworkDiagnostics offline(uint32_t now) {
  NetworkDiagnostics d;
  d.phase=NetworkPhase::Backoff; d.sequence=now; d.startedAtMs=now; d.progressAtMs=now;
  d.deadlineMs=20000;
  return d;
}
static NetworkDiagnostics online(uint32_t now) {
  auto d=offline(now); d.wifiConnected=true; return d;
}
// Associates at `at`, loses the link 1ms later and is due exactly 10s after.
static bool lostLinkRestarts(NetworkRestartHistory& h, uint32_t at) {
  NetworkRestartPolicy policy;
  assert(!policy.poll(h,online(at),at).restart);
  assert(!policy.poll(h,offline(at+1U),at+1U).restart);
  assert(!policy.poll(h,offline(at+10000U),at+10000U).restart);
  const auto r=policy.poll(h,offline(at+10001U),at+10001U);
  return r.restart && r.cause==3;
}
static void retainedCountAndSaturation() {
  NetworkRestartHistory h{}; NetworkRestartPolicy::boot(h,false,false);
  assert(h.magic==NetworkRestartPolicy::kMagic && h.attempts==0);
  for (unsigned expected=1;expected<=300;++expected) {
    assert(lostLinkRestarts(h,0)); // Every repeat is due at the same 10s.
    assert(h.attempts==expected && h.requested);
    NetworkRestartPolicy::boot(h,true,false);
    assert(h.attempts==expected && !h.requested);
  }
  assert(NetworkRestartPolicy::diagnosticAttempt(254)==254);
  assert(NetworkRestartPolicy::diagnosticAttempt(255)==255);
  assert(NetworkRestartPolicy::diagnosticAttempt(256)==255);
  assert(NetworkRestartPolicy::diagnosticAttempt(UINT32_MAX)==255);
  h.attempts=UINT32_MAX;
  assert(lostLinkRestarts(h,0));
  assert(h.attempts==UINT32_MAX && h.requested);
  NetworkRestartPolicy::boot(h,true,false); assert(h.attempts==UINT32_MAX);
  NetworkRestartPolicy::boot(h,false,true); assert(h.attempts==UINT32_MAX);
  h={NetworkRestartPolicy::kMagic,256,false}; NetworkRestartPolicy::boot(h,false,true);
  assert(h.attempts==257); // Actual watchdog boots count beyond 3/255 too.
  NetworkRestartPolicy::boot(h,false,false); assert(h.attempts==0); // Cold boot.
  h={0x47575232U,3,true}; NetworkRestartPolicy::boot(h,true,false);
  assert(h.magic==NetworkRestartPolicy::kMagic && h.attempts==0); // Old layout policy invalidated.
}
static void thresholdsIgnoreHistory() {
  for (uint32_t attempts : {0U,1U,2U,3U,99U}) {
    NetworkRestartHistory h{NetworkRestartPolicy::kMagic,attempts,false};
    assert(lostLinkRestarts(h,5000) && h.attempts==attempts+1);
    h={NetworkRestartPolicy::kMagic,attempts,false}; NetworkRestartPolicy never;
    assert(!never.poll(h,offline(0),0).restart); // Never associated this boot.
    assert(!never.poll(h,offline(179999),179999).restart);
    assert(never.poll(h,offline(180000),180000).restart);
    h={NetworkRestartPolicy::kMagic,attempts,false}; NetworkRestartPolicy stall;
    auto d=online(0);d.phase=NetworkPhase::Http;
    assert(!stall.poll(h,d,19999).restart);
    const auto r=stall.poll(h,d,20000);
    assert(r.restart && r.cause==2 && h.requested);
  }
}
static void ownOutageClock() {
  // Up for ten minutes, then down: the limit is counted from the first down
  // observation, not from boot or an earlier outage.
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,0,false}; NetworkRestartPolicy policy;
  for (uint32_t now=0;now<=600000;now+=1000) assert(!policy.poll(h,online(now),now).restart);
  assert(!policy.poll(h,offline(600100),600100).restart);
  assert(!policy.poll(h,offline(610099),610099).restart);
  assert(policy.poll(h,offline(610100),610100).restart && h.attempts==1);
  // A reconnection in between starts a new outage.
  h={NetworkRestartPolicy::kMagic,0,false}; NetworkRestartPolicy flap;
  flap.poll(h,online(0),0); flap.poll(h,offline(1000),1000); flap.poll(h,offline(9000),9000);
  assert(!flap.poll(h,online(9500),9500).restart);
  assert(!flap.poll(h,offline(10000),10000).restart);
  assert(!flap.poll(h,offline(19999),19999).restart);
  assert(flap.poll(h,offline(20000),20000).restart);
}
static void recoveryOtaAndCause() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,1,false}; NetworkRestartPolicy policy;
  policy.poll(h,online(0),0); policy.poll(h,offline(1000),1000);
  auto r=policy.poll(h,online(9000),9000);
  assert(!r.restart && !r.cause && h.attempts==1); // Link back before the limit.
  policy.poll(h,offline(10000),10000);
  r=policy.poll(h,offline(50000),50000,true);
  assert(!r.restart && h.attempts==1); // Pair-level OTA guard, not just worker phase.
  auto ota=offline(60000);ota.phase=NetworkPhase::Ota;
  assert(!policy.poll(h,ota,60000).restart);
  // The outage kept running under the OTA guard and is due once the guard ends.
  assert(policy.poll(h,offline(60001),60001).restart && h.attempts==2);
  h={NetworkRestartPolicy::kMagic,1,false}; NetworkRestartPolicy both;
  both.poll(h,online(0),0); both.poll(h,offline(1000),1000);
  auto d=offline(50000);d.phase=NetworkPhase::Http;d.startedAtMs=30000;
  r=both.poll(h,d,50000);
  assert(r.restart && r.cause==2); // A stalled worker outranks the link.
}
static void stableMeansConnectedAndProgressing() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,99,false}; NetworkRestartPolicy policy;
  // Reconnect attempts that keep advancing while disconnected cannot restore.
  for (uint32_t now=0;now<=600000;now+=1000)
    assert(!policy.poll(h,offline(now),now).restored);
  h={NetworkRestartPolicy::kMagic,99,false};
  // Ten connected minutes with real progress clear the count.
  for (uint32_t now=601000;now<1201000;now+=1000)
    assert(!policy.poll(h,online(now),now).restored);
  auto r=policy.poll(h,online(1201000),1201000);
  assert(r.restored && r.previousAttempts==99 && h.attempts==0);
  h.attempts=5;NetworkRestartPolicy frozen;
  auto d=online(0);d.deadlineMs=0; // Even stale diagnostics without a deadline cannot earn reset.
  for(uint32_t now=0;now<1800000;now+=1000) assert(!frozen.poll(h,d,now).restored);
  assert(h.attempts==5);
  NetworkRestartPolicy interrupted;
  for(uint32_t now=0;now<400000;now+=1000) interrupted.poll(h,online(now),now);
  interrupted.poll(h,offline(400000),400000);
  assert(!interrupted.poll(h,online(601000),601000).restored);
}
static void rollover() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,1,false};
  assert(lostLinkRestarts(h,UINT32_MAX-5000U));
}
int main() {
  retainedCountAndSaturation(); thresholdsIgnoreHistory(); ownOutageClock();
  recoveryOtaAndCause(); stableMeansConnectedAndProgressing(); rollover();
  puts("PASS: restart due at its own limit whatever the count, outage counted from the first down observation, >255/saturation, recovery/OTA guard, true link health and wrap");
}
