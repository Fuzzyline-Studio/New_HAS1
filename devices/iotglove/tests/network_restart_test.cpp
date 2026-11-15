#include "network_restart.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using namespace iotglove;
static NetworkDiagnostics offline(uint32_t now, bool wasUp=true, uint32_t since=0) {
  NetworkDiagnostics d;
  d.phase=NetworkPhase::Backoff; d.sequence=now; d.startedAtMs=now; d.progressAtMs=now;
  d.deadlineMs=20000; d.wifiWasUp=wasUp; d.wifiDownSinceMs=since;
  return d;
}
static NetworkDiagnostics online(uint32_t now) {
  auto d=offline(now); d.wifiConnected=true; return d;
}
static void retainedCountAndSaturation() {
  NetworkRestartHistory h{}; NetworkRestartPolicy::boot(h,false,false);
  assert(h.magic==NetworkRestartPolicy::kMagic && h.attempts==0);
  for (unsigned expected=1;expected<=300;++expected) {
    NetworkRestartPolicy policy;
    assert(!policy.poll(h,offline(29999),29999).restart);
    const auto r=policy.poll(h,offline(30000),30000);
    assert(r.restart && r.cause==3); // Every repeat is due at the same 30s.
    assert(h.attempts==expected && h.requested);
    NetworkRestartPolicy::boot(h,true,false);
    assert(h.attempts==expected && !h.requested);
  }
  assert(NetworkRestartPolicy::diagnosticAttempt(254)==254);
  assert(NetworkRestartPolicy::diagnosticAttempt(255)==255);
  assert(NetworkRestartPolicy::diagnosticAttempt(256)==255);
  assert(NetworkRestartPolicy::diagnosticAttempt(UINT32_MAX)==255);
  h.attempts=UINT32_MAX;
  NetworkRestartPolicy saturated;
  assert(saturated.poll(h,offline(30000),30000).restart);
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
    NetworkRestartPolicy lost;
    assert(!lost.poll(h,offline(29999),29999).restart);
    assert(lost.poll(h,offline(30000),30000).restart && h.attempts==attempts+1);
    h={NetworkRestartPolicy::kMagic,attempts,false}; NetworkRestartPolicy never;
    assert(!never.poll(h,offline(179999,false),179999).restart);
    assert(never.poll(h,offline(180000,false),180000).restart);
    h={NetworkRestartPolicy::kMagic,attempts,false}; NetworkRestartPolicy stall;
    auto d=online(0);d.phase=NetworkPhase::Http;
    assert(!stall.poll(h,d,19999).restart);
    const auto r=stall.poll(h,d,20000);
    assert(r.restart && r.cause==2 && h.requested);
  }
}
static void recoveryOtaAndCause() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,1,false}; NetworkRestartPolicy policy;
  assert(!policy.poll(h,offline(29999),29999).restart);
  auto r=policy.poll(h,online(50000),50000);
  assert(!r.restart && !r.cause && h.attempts==1); // Link back before the limit.
  // A forced reconnect publishes the moment it released the link, not the boot.
  assert(!policy.poll(h,offline(300100,true,300000),300100).restart);
  assert(!policy.poll(h,offline(329999,true,300000),329999).restart);
  r=policy.poll(h,offline(350000,true,300000),350000,true);
  assert(!r.restart && h.attempts==1); // Pair-level OTA guard, not just worker phase.
  auto ota=offline(2000000);ota.phase=NetworkPhase::Ota;
  assert(!policy.poll(h,ota,2000000).restart);
  auto d=offline(50000);d.phase=NetworkPhase::Http;d.startedAtMs=30000;
  r=policy.poll(h,d,50000);
  assert(r.restart && r.cause==2 && h.attempts==2); // A stalled worker outranks the link.
}
static void stableMeansConnectedAndProgressing() {
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,99,false}; NetworkRestartPolicy policy;
  // Reconnect attempts that keep advancing while disconnected cannot restore.
  for (uint32_t now=0;now<=600000;now+=1000) {
    auto r=policy.poll(h,offline(now,true,now),now);assert(!r.restored && !r.restart);
  }
  assert(h.attempts==99);
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
  interrupted.poll(h,offline(400000,true,400000),400000);
  assert(!interrupted.poll(h,online(601000),601000).restored);
}
static void rollover() {
  const uint32_t start=UINT32_MAX-10000U;
  NetworkRestartHistory h{NetworkRestartPolicy::kMagic,1,false};NetworkRestartPolicy p;
  assert(!p.poll(h,offline(start+29999U,true,start),start+29999U).restart);
  assert(p.poll(h,offline(start+30000U,true,start),start+30000U).restart);
}
int main() {
  retainedCountAndSaturation(); thresholdsIgnoreHistory();
  recoveryOtaAndCause(); stableMeansConnectedAndProgressing(); rollover();
  puts("PASS: restart due at its own limit whatever the count, >255/saturation, recovery/OTA guard, forced-reconnect clock, true link health and wrap");
}
