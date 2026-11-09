#!/usr/bin/env python3
"""Run production RTC journal and logger with fake time/reset and a memory sink."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PREAMBLE=r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "generator_diagnostics.h"
#define RTC_NOINIT_ATTR
#define FIRMWARE_VER 26
static unsigned criticalDepth=0;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do { (void)x; assert(++criticalDepth==1); } while(0)
#define portEXIT_CRITICAL(x) do { (void)x; assert(--criticalDepth==0); } while(0)
void BlackBoxDiagnostic(uint16_t,uint8_t,uint32_t,int32_t,uint32_t,uint32_t) { assert(criticalDepth==0); }
namespace has1blackbox {
 enum class EventType {Crash,Progress};
 struct Captured {int32_t a,b,c,d;std::string text;EventType type;};
 static std::vector<Captured> captured;
 static unsigned rejectNext=0; // simulate a full logger queue
 struct Logger {uint32_t log(EventType type,int32_t a,int32_t b,int32_t c,int32_t d,const char* text) {
  assert(criticalDepth==0);
  if(rejectNext){--rejectNext;return 0;}
  captured.push_back({a,b,c,d,text,type});return uint32_t(captured.size());
 }} blackbox;
}
static bool audioQuiet=true;
bool BlackBoxTraceAudioQuiet(uint32_t,uint32_t ms) { assert(ms>=100); return audioQuiet; }
static uint32_t clockMs=0;
unsigned long millis() { return clockMs; }
enum esp_reset_reason_t { ESP_RST_UNKNOWN,ESP_RST_POWERON,ESP_RST_EXT,ESP_RST_SW,
 ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_DEEPSLEEP,ESP_RST_BROWNOUT,ESP_RST_SDIO };
static esp_reset_reason_t resetReason=ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason() { return resetReason; }
struct FakeEsp { uint32_t getFreeHeap(){return 100000;} uint32_t getMinFreeHeap(){return 90000;}
 uint32_t getMaxAllocHeap(){return 80000;} } ESP;
struct FakeWifi { int status(){return 3;} int RSSI(){return -60;} } WiFi;
struct FakeShift { int watchdog=0; int operator[](const char *)const{return watchdog;} } shift_machine;
static std::string output;
static size_t largestWrite=0;
void TelnetDiagnosticWrite(const char *text,size_t size) {
 assert(criticalDepth==0 && size<=319); largestWrite=std::max(largestWrite,size); output.append(text,size);
}
'''
TESTS=r'''
using namespace GeneratorDiagnostic;
static Record record(uint32_t seq) {
 Record r={}; r.sequence=seq;r.at=seq*10;r.id=seq;r.detail=99;r.event=Neo;r.phase=Begin;return r;
}
static void journalTests() {
 volatile Journal j={}; Record prior[Capacity]; unsigned invalid=0;
 initialize(j,26); assert(recover(j,26,true,prior,invalid)==0);
 for(unsigned n=1;n<=100;++n) append(j,record(n));
 assert(recover(j,26,true,prior,invalid)==32 && prior[0].sequence==69 && prior[31].sequence==100);
 assert(recover(j,26,false,prior,invalid)==0 && recover(j,27,true,prior,invalid)==0);
 j.schema=99; assert(recover(j,26,true,prior,invalid)==0); j.schema=Schema;
 j.checksum^=1; assert(recover(j,26,true,prior,invalid)==0); j.checksum=headerChecksum(26);
 puts("PASS 32-record chronological wrap; cold/firmware/schema/header corruption rejected");
 j.records[99%Capacity].detail^=1;
 assert(recover(j,26,true,prior,invalid)==31 && invalid==1);
 j.records[98%Capacity].checksum=0; j.records[98%Capacity].sequence=200; // Reset during commit.
 assert(recover(j,26,true,prior,invalid)==30);
 puts("PASS corrupt payload and torn writes ignored without trusting indexes");
 initialize(j,26);
 for(uint32_t seq=UINT32_MAX-10;seq!=0;++seq) append(j,record(seq));
 for(uint32_t seq=1;seq<=10;++seq) append(j,record(seq));
 const unsigned count=recover(j,26,true,prior,invalid);
 assert(count==21 && prior[0].sequence==UINT32_MAX-10 && prior[count-1].sequence==10);
 puts("PASS sequence wrap and nonzero sequence ordering");
 RateGate gate; assert(gate.allow(UINT32_MAX-50,500));
 assert(!gate.allow(100,500)); assert(gate.allow(500,500)); assert(gate.skipped==1);
 puts("PASS rate limits across millis rollover; denied events do not push deadline");
}
static void runtimeTests() {
 initialize(diagnosticRtc,26);
 for(unsigned i=1;i<=32;++i) {Record r=record(i);if(i==3)r.code=-42;append(diagnosticRtc,r);}
 resetReason=ESP_RST_SW;has1blackbox::captured.clear();DiagnosticInit();
 std::vector<int32_t> selected;
 bool coverage=false;
 for(const auto &r:has1blackbox::captured) {
  if(r.text=="prior.rtc.event")selected.push_back(r.a);
  if(r.text=="prior.rtc.coverage")coverage=r.a==32 && r.b==0 && r.c==24 && r.d==1;
 }
 assert(coverage && selected.size()==8 && selected.front()==3 && selected[1]==26 && selected.back()==32);
 puts("PASS earliest retained RTC fault plus newest seven; omission count explicit");
 {
  // The other 24 follow one per step after the boot delay, never as a burst.
  auto count=[](const char *text){size_t n=0;for(const auto &r:has1blackbox::captured)if(r.text==text)++n;return n;};
  const uint32_t booted=clockMs;
  clockMs=booted+9999; for(unsigned i=0;i<5;++i) DiagnosticLoop();
  assert(count("prior.rtc.event")==8 && count("prior.rtc.time")==8);
  audioQuiet=false; clockMs=booted+10000; for(unsigned i=0;i<5;++i) DiagnosticLoop();
  assert(count("prior.rtc.event")==8); // paused near DFPlayer traffic
  audioQuiet=true; has1blackbox::rejectNext=1; DiagnosticLoop();
  assert(count("prior.rtc.event")==8); // queue full: retried, not marked logged
  clockMs=booted+10499; DiagnosticLoop(); assert(count("prior.rtc.event")==8);
  clockMs=booted+10500; for(unsigned i=0;i<5;++i) DiagnosticLoop();
  assert(count("prior.rtc.event")==9 && count("prior.rtc.time")==9);
  for(unsigned step=1;step<=40;++step){clockMs=booted+10500+step*500;DiagnosticLoop();DiagnosticLoop();}
  std::vector<int32_t> all;
  bool ended=false, types=true;
  for(const auto &r:has1blackbox::captured) {
   if(r.text=="prior.rtc.event")all.push_back(r.a);
   if(r.text=="prior.rtc.more.end")ended=r.a==24 && r.b==32 && r.c==1 && r.d==0;
   if(r.text.rfind("prior.rtc.",0)==0 && r.text!="prior.rtc.coverage") {
    const bool boot=std::find(selected.begin(),selected.end(),r.a)!=selected.end() && r.text!="prior.rtc.more.end";
    types=types && r.type==(boot?has1blackbox::EventType::Crash:has1blackbox::EventType::Progress);
   }
  }
  std::sort(all.begin(),all.end());
  assert(all.size()==32 && ended && types && count("prior.rtc.time")==32 && count("prior.rtc.more.end")==1);
  for(unsigned i=0;i<32;++i) assert(all[i]==int32_t(i+1));
  puts("PASS remaining 24 replayed once each as Progress, 500 ms apart, paused near audio, retried when rejected");
 }
 {
  // Eight or fewer events: everything goes at boot and no later records follow.
  initialize(diagnosticRtc,26);
  for(unsigned i=1;i<=5;++i) append(diagnosticRtc,record(i));
  resetReason=ESP_RST_SW; has1blackbox::captured.clear(); const uint32_t booted=clockMs; DiagnosticInit();
  for(unsigned step=0;step<60;++step){clockMs=booted+step*500;DiagnosticLoop();}
  size_t events=0,ends=0;
  for(const auto &r:has1blackbox::captured){events+=r.text=="prior.rtc.event";ends+=r.text=="prior.rtc.more.end";}
  assert(events==5 && ends==0);
  puts("PASS small journals replay entirely at boot without a later summary");
 }
 resetReason=ESP_RST_POWERON; DiagnosticInit(); output.clear();
 DiagnosticHttpHook("Loop","request.begin",0,0,0);
 DiagnosticHttpHook("Loop","get.end",200,700,0);
 DiagnosticHttpHook("Loop","body.end",0,800,10);
 DiagnosticHttpHook("Loop","close.begin",200,800,10);
 // Simulate a reset while close is pending: preserve records, reset ordinary globals.
 resetReason=ESP_RST_TASK_WDT; diagnosticSequence=diagnosticId=0;
 DiagnosticInit(); assert(diagnosticPriorCount>=6);
 std::string previous;
 char line[320];
 for(unsigned n=0;;++n){size_t len=DiagnosticPriorLine(n,line,sizeof(line));if(!len)break;previous.append(line,len);}
 assert(previous.find("stage=close.begin kind=Loop")!=std::string::npos);
 assert(previous.find("has2_http BEGIN")!=std::string::npos);
 for(unsigned n=0;n<200;++n) DiagnosticPoint(Wire,n,n);
 std::string again;
 for(unsigned n=0;;++n){size_t len=DiagnosticPriorLine(n,line,sizeof(line));if(!len)break;again.append(line,len);}
 assert(previous==again);
 puts("PASS reset during close retains stage/kind; previous snapshot survives current journal overwrite and reconnect");
 // SDIO and EN-glitch POWERON keep RTC memory: recover it, report the cold class.
 for (esp_reset_reason_t cold : {ESP_RST_SDIO,ESP_RST_POWERON,ESP_RST_BROWNOUT}) {
  for(unsigned n=0;n<4;++n) DiagnosticPoint(Wire,n,n);
  resetReason=cold; has1blackbox::captured.clear(); DiagnosticInit();
  bool coldCoverage=false;
  for(const auto &r:has1blackbox::captured)
   if(r.text=="prior.rtc.coverage")coldCoverage=r.a>0 && r.d==0;
  assert(diagnosticPriorCount>0 && !diagnosticPriorWarm && coldCoverage);
 }
 puts("PASS SDIO/power-on/brownout with intact RTC journal are recovered and marked cold");
 // A real power loss leaves arbitrary RTC contents: no header, no records.
 for (unsigned pattern : {0x00u,0xffu,0xa5u,0x5au}) {
  std::memset(const_cast<GeneratorDiagnostic::Journal*>(&diagnosticRtc),int(pattern),sizeof(diagnosticRtc));
  resetReason=ESP_RST_POWERON; DiagnosticInit(); assert(diagnosticPriorCount==0);
 }
 {
  uint32_t lcg=12345;
  unsigned char *bytes=reinterpret_cast<unsigned char*>(const_cast<GeneratorDiagnostic::Journal*>(&diagnosticRtc));
  for(unsigned trial=0;trial<200;++trial) {
   for(size_t i=0;i<sizeof(diagnosticRtc);++i){lcg=lcg*1103515245u+12345u;bytes[i]=uint8_t(lcg>>16);}
   resetReason=ESP_RST_POWERON; DiagnosticInit(); assert(diagnosticPriorCount==0);
  }
  // A valid header with garbage records still yields no records.
  initialize(diagnosticRtc,26);
  for(size_t i=offsetof(GeneratorDiagnostic::Journal,records);i<sizeof(diagnosticRtc);++i){lcg=lcg*1103515245u+12345u;bytes[i]=uint8_t(lcg>>16);}
  resetReason=ESP_RST_SDIO; DiagnosticInit(); assert(diagnosticPriorCount==0);
 }
 puts("PASS power-loss garbage (fixed patterns, 200 random images, garbage records) recovers nothing");
 output.clear(); diagnosticNormal[Neo]=RateGate(); diagnosticErrors[Neo]=RateGate();
 clockMs=1000;
 for(unsigned i=0;i<1000;++i){DiagnosticScope scope(Neo,2);scope.result(-1,2);}
 size_t lines=0;for(char c:output)if(c=='\n')++lines;
 assert(lines==2); clockMs=2000;{DiagnosticScope scope(Neo,2);scope.result(-1,2);}
 assert(output.find("suppressed=999")!=std::string::npos);
 puts("PASS 1000 repeated faults bounded to one BEGIN/END pair per second and suppression summary");
 output.clear(); diagnosticNormal[Has2Http]=RateGate(); diagnosticErrors[Has2Http]=RateGate();
 DiagnosticHttpHook("Receive","request.begin",0,0,0);
 DiagnosticHttpHook("Receive","request.end",1,1,10);
 DiagnosticHttpHook("Receive","request.begin",0,0,0);
 DiagnosticHttpHook("Receive","request.end",0,1,0);
 assert(output.find("END code=-1")!=std::string::npos);
 puts("PASS fast HTTP failures remain visible even after normal trace throttling");
 shift_machine.watchdog=1;
 DiagnosticHttpHook("Loop","request.begin",0,0,0);
 DiagnosticHttpHook("Loop","request.end",1,1,10);
 assert(output.find("server_watchdog=1 observed")!=std::string::npos);
 Record records[Capacity];unsigned invalid=0;
 const unsigned n=recover(diagnosticRtc,26,true,records,invalid);
 assert(n && records[n-1].event==ServerReset && records[n-1].detail==1);
 shift_machine.watchdog=0;
 puts("PASS successful server watchdog flag retained separately from hardware watchdog reset reason");
 output.clear(); DiagnosticPrintf("%s",std::string(5000,'A').c_str());
 assert(output.size()==319 && output.substr(317)=="\r\n" && largestWrite<=319);
 puts("PASS formatter length bounded and truncation remains line-delimited; sink outside every lock");
}
int main(){journalTests();runtimeTests();return 0;}
'''
source=(ROOT/'diagnostics.ino').read_text()
# Any UART/heap/network dependency introduced into the diagnostic core must be reviewed.
for token in ('Serial.', 'DebugSerial.', 'malloc(', 'new ', 'send(', '.write(', 'nvs_'):
    assert token not in source, f'unexpected blocking/allocation path: {token}'
with tempfile.TemporaryDirectory(prefix='generator-diagnostic-test-') as folder:
    d=Path(folder)
    (d/'esp_system.h').write_text('// reset reason stubs in test preamble\n')
    (d/'esp_attr.h').write_text('// RTC attribute stub in test preamble\n')
    (d/'test.cpp').write_text(PREAMBLE+source+TESTS)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror',
      '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(d),'-I',str(ROOT),
      str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
