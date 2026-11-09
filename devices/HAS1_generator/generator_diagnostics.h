#ifndef GENERATOR_DIAGNOSTICS_H
#define GENERATOR_DIAGNOSTICS_H
#include <stdint.h>
#include <stddef.h>
#include "generator_trace.h"

// POD only: RTC_NOINIT storage must not receive a C++ startup initializer.
namespace GeneratorDiagnostic {
static const uint32_t Magic = 0x47444941;
static const uint32_t Schema = 1;
static const unsigned Capacity = 32;
enum Event : uint16_t { Boot = 1, LoopGap, Health, Wire, Neo, AudioSend,
  AudioDrain, AudioWait, WireHttp, Has2Http, CrashUpload, ServerReset, LastEvent=ServerReset };
enum Phase : uint8_t { Begin = 1, End, Point };
struct Record {
  uint32_t sequence, at, id, detail, elapsed;
  int32_t code;
  uint16_t event;
  uint8_t phase, context;
  uint32_t checksum;
};
struct Journal {
  uint32_t magic, schema, firmware, checksum;
  Record records[Capacity];
};
static_assert(sizeof(Record) == 32, "RTC record layout changed");
static_assert(sizeof(Journal) <= 1600, "RTC journal budget exceeded");
inline uint32_t mix(uint32_t h, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) { h = (h ^ (value & 255)) * 16777619u; value >>= 8; }
  return h;
}
inline uint32_t headerChecksum(uint32_t firmware) {
  return mix(mix(mix(2166136261u, Magic), Schema), firmware) | 1u;
}
inline uint32_t checksum(const Record &r) {
  uint32_t h = 2166136261u;
  h=mix(h,r.sequence); h=mix(h,r.at); h=mix(h,r.id); h=mix(h,r.detail);
  h=mix(h,r.elapsed); h=mix(h,static_cast<uint32_t>(r.code));
  return mix(h, uint32_t(r.event) | (uint32_t(r.phase)<<16) | (uint32_t(r.context)<<24)) | 1u;
}
inline Record read(const volatile Record &r) {
  Record out;
  out.sequence=r.sequence; out.at=r.at; out.id=r.id; out.detail=r.detail;
  out.elapsed=r.elapsed; out.code=r.code; out.event=r.event; out.phase=r.phase;
  out.context=r.context; out.checksum=r.checksum;
  return out;
}
inline bool valid(const Record &r) {
  return r.sequence && r.event >= Boot && r.event <= LastEvent &&
      r.phase >= Begin && r.phase <= Point && (r.context & 15)<=5 && (r.context >> 4)<=5 && r.checksum == checksum(r);
}
inline void initialize(volatile Journal &j, uint32_t firmware) {
  j.checksum=0; j.magic=Magic; j.schema=Schema; j.firmware=firmware;
  for (unsigned i=0;i<Capacity;++i) j.records[i].checksum=0;
  __asm__ __volatile__("" ::: "memory");
  j.checksum=headerChecksum(firmware);
}
inline void append(volatile Journal &j, const Record &r) {
  volatile Record &target=j.records[(r.sequence-1)%Capacity];
  target.checksum=0; // Invalidate the overwritten slot before any partial update.
  __asm__ __volatile__("" ::: "memory");
  target.sequence=r.sequence; target.at=r.at; target.id=r.id; target.detail=r.detail;
  target.elapsed=r.elapsed; target.code=r.code; target.event=r.event;
  target.phase=r.phase; target.context=r.context;
  __asm__ __volatile__("" ::: "memory");
  target.checksum=checksum(r); // Commit last; reset midway leaves an invalid slot.
}
// Called only at boot before producers. Copies newest valid records in order;
// never derives an address/count from untrusted retained data.
inline unsigned recover(const volatile Journal &j, uint32_t firmware, bool warm,
                        Record (&out)[Capacity], unsigned &invalid) {
  invalid=0;
  if (!warm || j.magic!=Magic || j.schema!=Schema || j.firmware!=firmware ||
      j.checksum!=headerChecksum(firmware)) return 0;
  uint32_t newest=0;
  for (unsigned i=0;i<Capacity;++i) {
    const Record r=read(j.records[i]);
    if (!r.checksum) continue;
    if (!valid(r)) { ++invalid; continue; }
    if (!newest || static_cast<int32_t>(r.sequence-newest)>0) newest=r.sequence;
  }
  unsigned count=0;
  for (unsigned age=Capacity;age>0;--age) {
    const uint32_t seq=newest-(age-1);
    if (!seq) continue;
    const Record r=read(j.records[(seq-1)%Capacity]);
    if (r.sequence==seq && valid(r)) out[count++]=r;
  }
  return count;
}
// Unsigned arithmetic deliberately supports millis rollover. A denied event
// does not move the deadline, so a continuous flood cannot suppress forever.
struct RateGate {
  uint32_t last=0, skipped=0;
  bool seen=false;
  bool allow(uint32_t now, uint32_t interval) {
    if (seen && uint32_t(now-last)<interval) { ++skipped; return false; }
    seen=true; last=now; return true;
  }
};
}

// Flash blackbox trace of plug/audio transitions (blackbox.ino, generator_trace.h).
// Captured in RAM and logged later, away from DFPlayer UART traffic.
void BlackBoxTrace(uint8_t kind,int32_t a,int32_t b,int32_t c);
bool BlackBoxTraceAudioQuiet(uint32_t now,uint32_t ms);

void TelnetDiagnosticWrite(const char *text, size_t length);
void DiagnosticInit();
void DiagnosticLoop();
void DiagnosticPrintf(const char *format, ...);
uint32_t DiagnosticBegin(uint16_t event, uint32_t detail, bool &verbose);
void DiagnosticEnd(uint16_t event,uint32_t id,uint32_t started,int32_t code,uint32_t detail,bool verbose);
void DiagnosticPoint(uint16_t event,int32_t code,uint32_t detail);
void DiagnosticHttpHook(const char *request,const char *phase,int32_t result,uint32_t elapsed,uint32_t detail);
size_t DiagnosticPriorLine(unsigned index,char *buffer,size_t capacity);
class DiagnosticScope {
 public:
  DiagnosticScope(uint16_t event,uint32_t detail=0);
  ~DiagnosticScope();
  void result(int32_t code,uint32_t detail=0) { code_=code; detail_=detail; }
  void phase(const char *name,int32_t code,uint32_t detail=0);
 private:
  uint16_t event_;
  uint32_t id_,started_,detail_;
  int32_t code_;
  bool verbose_;
  DiagnosticScope(const DiagnosticScope &) = delete;
  DiagnosticScope &operator=(const DiagnosticScope &) = delete;
};
#endif
