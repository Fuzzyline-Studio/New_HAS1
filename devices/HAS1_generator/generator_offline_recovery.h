#pragma once
#include <stdint.h>

// Device-local policy. Only missing Wi-Fi association counts as an outage;
// HTTP errors and a stalled worker are handled by their existing paths.
namespace GeneratorOfflineRecovery {
constexpr uint32_t Magic=0x474f5231u; // GOR1: change when the retained layout changes.
constexpr uint32_t StableMs=600000, ServerFreshMs=15000;
struct Retained {
  uint32_t magic,attempts,outageMs,pending,check;
};
inline uint32_t checksum(const Retained &s) {
  return s.magic^s.attempts^s.outageMs^s.pending^0x6a329fc1u;
}
inline void seal(Retained &s) {s.magic=Magic;s.check=checksum(s);}
inline bool valid(const Retained &s) {
  return s.magic==Magic&&s.attempts<=65535&&s.pending<=1&&s.check==checksum(s);
}
// Keep history only for our own requested software reset, not unrelated resets.
inline bool boot(Retained &s,bool softwareReset) {
  const bool previous=softwareReset&&valid(s)&&s.pending==1;
  if(!previous) s=Retained{};
  s.pending=0;seal(s);return previous;
}
inline uint32_t delayMs(uint32_t attempts,uint32_t deviceSalt,bool hadConnection=false) {
  // Restore a previously working link promptly; allow more time for first boot
  // association. Own repeated recovery boots retain the slower storm backoff.
  const uint32_t initial=hadConnection?30000u:180000u;
  const uint32_t base=attempts==0?initial:attempts==1?300000u:attempts==2?900000u:1800000u;
  return base+(deviceSalt%5001u);
}
inline void requested(Retained &s,uint32_t elapsed) {
  if(s.attempts<65535) ++s.attempts;
  s.outageMs=elapsed;s.pending=1;seal(s);
}
// A latched scan driver fault leaves the worker stopped for good, so waiting out
// the outage delay gains nothing: restart at once. A fault straight after an own
// recovery restart keeps the worker-watchdog fail-stop as the slower path.
inline bool faultRestartDue(const Retained &history) {return history.attempts==0;}
struct Policy {
  bool offline=false,stable=false,hasServer=false,hadConnection=false;
  uint32_t offlineSince=0,stableSince=0,serverAt=0;
  void serverReply(uint32_t now){hasServer=true;serverAt=now;}
  bool sample(uint32_t now,bool online,Retained &history) {
    if(!online) {
      stable=false;
      if(!offline){offline=true;offlineSince=now;}
      return false;
    }
    offline=false;hadConnection=true; // Per-boot observation, never retained in RTC.
    if(!hasServer||uint32_t(now-serverAt)>=ServerFreshMs){stable=false;return false;}
    if(!stable){stable=true;stableSince=now;}
    if(history.attempts&&uint32_t(now-stableSince)>=StableMs) {
      history=Retained{};seal(history);return true;
    }
    return false;
  }
  uint32_t elapsed(uint32_t now)const{return offline?uint32_t(now-offlineSince):0;}
  bool due(uint32_t now,const Retained &history,uint32_t salt)const {
    return offline&&elapsed(now)>=delayMs(history.attempts,salt,hadConnection);
  }
};
} // namespace GeneratorOfflineRecovery
