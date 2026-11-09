#include "generator_diagnostics.h"
#include <esp_system.h>
#include <esp_attr.h>
#include <stdarg.h>

// Never initialized by startup. Recovered after every reset reason: field
// SDIO and EN-glitch POWERON resets keep RTC memory, while a real power loss
// leaves no valid header/record checksums. `warm` only reports the reset class.
// RTC is evidence of the previous boot, not durable crash storage.
RTC_NOINIT_ATTR static volatile GeneratorDiagnostic::Journal diagnosticRtc;
static GeneratorDiagnostic::Record diagnosticPrior[GeneratorDiagnostic::Capacity];
static unsigned diagnosticPriorCount=0, diagnosticPriorInvalid=0;
static bool diagnosticPriorWarm=false;
// Events not in the boot-time eight are replayed later from DiagnosticLoop.
static bool diagnosticPriorLogged[GeneratorDiagnostic::Capacity];
static unsigned diagnosticPriorNext=0, diagnosticPriorLater=0, diagnosticPriorTimeLost=0;
static uint32_t diagnosticPriorReplayAt=0;
static bool diagnosticPriorReplayDone=true;
static const uint32_t DiagnosticPriorReplayDelayMs=10000, DiagnosticPriorReplayStepMs=500,
    DiagnosticPriorQuietMs=300;
static portMUX_TYPE diagnosticMux=portMUX_INITIALIZER_UNLOCKED;
static uint32_t diagnosticSequence=0, diagnosticId=0;
static GeneratorDiagnostic::RateGate diagnosticNormal[GeneratorDiagnostic::LastEvent+1];
static GeneratorDiagnostic::RateGate diagnosticErrors[GeneratorDiagnostic::LastEvent+1];

static const char *DiagnosticName(uint16_t event) {
  switch(event) {
    case GeneratorDiagnostic::Boot: return "boot";
    case GeneratorDiagnostic::LoopGap: return "loop_gap";
    case GeneratorDiagnostic::Health: return "health";
    case GeneratorDiagnostic::Wire: return "wire";
    case GeneratorDiagnostic::Neo: return "neo_tx";
    case GeneratorDiagnostic::AudioSend: return "audio_send";
    case GeneratorDiagnostic::AudioDrain: return "audio_drain";
    case GeneratorDiagnostic::AudioWait: return "audio_wait";
    case GeneratorDiagnostic::WireHttp: return "wire_http";
    case GeneratorDiagnostic::Has2Http: return "has2_http";
    case GeneratorDiagnostic::CrashUpload: return "crash_upload";
    case GeneratorDiagnostic::ServerReset: return "server_watchdog";
    default: return "unknown";
  }
}
void DiagnosticPrintf(const char *format, ...) {
  char line[320];
  va_list args; va_start(args,format);
  const int length=vsnprintf(line,sizeof(line),format,args);
  va_end(args);
  if (length<=0) return;
  size_t kept=static_cast<size_t>(length);
  if (kept>=sizeof(line)) { kept=sizeof(line)-1; line[kept-2]='\r'; line[kept-1]='\n'; }
  // Memory-only path: NEVER Serial/DebugSerial/Print or socket writes here.
  TelnetDiagnosticWrite(line,kept);
}
static void DiagnosticRecord(uint16_t event,uint8_t phase,uint32_t id,
                             int32_t code,uint32_t elapsed,uint32_t detail,uint8_t context) {
  GeneratorDiagnostic::Record r={};
  r.at=millis(); r.event=event; r.phase=phase; r.id=id;
  r.code=code; r.elapsed=elapsed; r.detail=detail; r.context=context;
  portENTER_CRITICAL(&diagnosticMux);
  if (!++diagnosticSequence) ++diagnosticSequence;
  r.sequence=diagnosticSequence;
  GeneratorDiagnostic::append(diagnosticRtc,r);
  portEXIT_CRITICAL(&diagnosticMux);
  BlackBoxDiagnostic(event,phase,id,code,elapsed,detail);
}
uint32_t DiagnosticBegin(uint16_t event,uint32_t detail,bool &verbose) {
  const uint32_t now=millis();
  uint32_t id;
  portENTER_CRITICAL(&diagnosticMux);
  if (!++diagnosticId) ++diagnosticId;
  id=diagnosticId;
  verbose=event<=GeneratorDiagnostic::LastEvent && diagnosticNormal[event].allow(now,1000);
  portEXIT_CRITICAL(&diagnosticMux);
  DiagnosticRecord(event,GeneratorDiagnostic::Begin,id,0,0,detail,0);
  if (verbose) DiagnosticPrintf("[GD] t=%lu id=%lu %s BEGIN detail=%lu\r\n",
    (unsigned long)now,(unsigned long)id,DiagnosticName(event),(unsigned long)detail);
  return id;
}
void DiagnosticEnd(uint16_t event,uint32_t id,uint32_t started,int32_t code,uint32_t detail,bool verbose) {
  const uint32_t elapsed=uint32_t(millis()-started);
  DiagnosticRecord(event,GeneratorDiagnostic::End,id,code,elapsed,detail,0);
  uint32_t skipped=0;
  if (code<0 || elapsed>=250) {
    portENTER_CRITICAL(&diagnosticMux);
    const bool allowed=diagnosticErrors[event].allow(millis(),1000);
    if (allowed) { skipped=diagnosticErrors[event].skipped; diagnosticErrors[event].skipped=0; }
    portEXIT_CRITICAL(&diagnosticMux);
    verbose=allowed;
  }
  if (verbose) DiagnosticPrintf("[GD] t=%lu id=%lu %s END code=%ld elapsed=%lu detail=%lu suppressed=%lu\r\n",
    (unsigned long)millis(),(unsigned long)id,DiagnosticName(event),(long)code,
    (unsigned long)elapsed,(unsigned long)detail,(unsigned long)skipped);
}
void DiagnosticPoint(uint16_t event,int32_t code,uint32_t detail) {
  DiagnosticRecord(event,GeneratorDiagnostic::Point,0,code,0,detail,0);
}
DiagnosticScope::DiagnosticScope(uint16_t event,uint32_t detail)
  :event_(event),id_(0),started_(millis()),detail_(detail),code_(0),verbose_(false) {
  id_=DiagnosticBegin(event,detail,verbose_);
}
DiagnosticScope::~DiagnosticScope() { DiagnosticEnd(event_,id_,started_,code_,detail_,verbose_); }
static uint8_t DiagnosticStage(const char *phase) {
  if (!strcmp(phase,"get.begin")) return 1;
  if (!strcmp(phase,"get.end")) return 2;
  if (!strcmp(phase,"body.end")) return 3;
  if (!strcmp(phase,"close.begin")) return 4;
  if (!strcmp(phase,"close.end")) return 5;
  return 0;
}
static const char *DiagnosticStageName(uint8_t context) {
  const char *names[]={"none","get.begin","get.end","body.end","close.begin","close.end"};
  return names[(context & 15)<=5 ? (context & 15):0];
}
static uint8_t DiagnosticRequestKind(const char *request) {
  if (!strcmp(request,"Loop")) return 1;
  if (!strcmp(request,"ReceiveMine")) return 2;
  if (!strcmp(request,"Receive")) return 3;
  if (!strcmp(request,"Send")) return 4;
  return 5;
}
static const char *DiagnosticRequestName(uint8_t kind) {
  const char *names[]={"none","Loop","ReceiveMine","Receive","Send","other"};
  return names[kind<=5 ? kind:5];
}
void DiagnosticScope::phase(const char *name,int32_t code,uint32_t detail) {
  // Phase points survive reset even if the loop never gets back to TelnetDrain.
  DiagnosticRecord(event_,GeneratorDiagnostic::Point,id_,code,uint32_t(millis()-started_),detail,DiagnosticStage(name));
  if (verbose_) DiagnosticPrintf("[GD] t=%lu id=%lu %s %s code=%ld elapsed=%lu detail=%lu\r\n",
    (unsigned long)millis(),(unsigned long)id_,DiagnosticName(event_),name,(long)code,
    (unsigned long)uint32_t(millis()-started_),(unsigned long)detail);
}
void DiagnosticInit() {
  const esp_reset_reason_t reason=esp_reset_reason();
  diagnosticPriorWarm=reason==ESP_RST_SW || reason==ESP_RST_PANIC ||
      reason==ESP_RST_INT_WDT || reason==ESP_RST_TASK_WDT || reason==ESP_RST_WDT ||
      reason==ESP_RST_DEEPSLEEP;
  diagnosticPriorCount=GeneratorDiagnostic::recover(diagnosticRtc,FIRMWARE_VER,
      true,diagnosticPrior,diagnosticPriorInvalid);
  has1blackbox::blackbox.log(has1blackbox::EventType::Crash,diagnosticPriorCount,
      diagnosticPriorInvalid,diagnosticPriorCount>8?diagnosticPriorCount-8:0,
      diagnosticPriorWarm,"prior.rtc.coverage");
  // Bound startup traffic to eight events. Preserve the earliest retained
  // fault plus the newest seven, rather than discarding every older fault.
  // This is only the prior 32-event RTC window, not indefinite first-fault pinning.
  unsigned selected[8], selectedCount=0;
  if(diagnosticPriorCount>8) {
    unsigned firstFault=0;
    for(unsigned n=0;n<diagnosticPriorCount;++n) {
      if(diagnosticPrior[n].code<0) {firstFault=n;break;}
    }
    if(firstFault<diagnosticPriorCount-7)selected[selectedCount++]=firstFault;
  }
  const unsigned tail=selectedCount?7:8;
  for(unsigned n=diagnosticPriorCount>tail?diagnosticPriorCount-tail:0;n<diagnosticPriorCount;++n)
    selected[selectedCount++]=n;
  for(unsigned n=0;n<GeneratorDiagnostic::Capacity;++n) diagnosticPriorLogged[n]=false;
  for(unsigned n=0;n<selectedCount;++n) {
    const auto &r=diagnosticPrior[selected[n]];
    diagnosticPriorLogged[selected[n]]=true;
    has1blackbox::blackbox.log(has1blackbox::EventType::Crash,
        r.sequence,(uint32_t(r.event)<<8)|r.phase,r.code,r.detail,"prior.rtc.event");
    has1blackbox::blackbox.log(has1blackbox::EventType::Crash,
        r.sequence,r.at,r.elapsed,r.id,"prior.rtc.time");
  }
  diagnosticPriorNext=diagnosticPriorLater=diagnosticPriorTimeLost=0;
  diagnosticPriorReplayAt=millis()+DiagnosticPriorReplayDelayMs;
  diagnosticPriorReplayDone=diagnosticPriorCount<=selectedCount;
  GeneratorDiagnostic::initialize(diagnosticRtc,FIRMWARE_VER);
  DiagnosticPoint(GeneratorDiagnostic::Boot,static_cast<int32_t>(reason),ESP.getFreeHeap());
  DiagnosticPrintf("[GD] BOOT fw=%u reset=%d heap=%lu min_heap=%lu prior_records=%u prior_invalid=%u warm=%u\r\n",
      (unsigned)FIRMWARE_VER,(int)reason,(unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getMinFreeHeap(),diagnosticPriorCount,diagnosticPriorInvalid,diagnosticPriorWarm);
}
size_t DiagnosticPriorLine(unsigned index,char *buffer,size_t capacity) {
  if (!buffer || !capacity) return 0;
  int length;
  if (index==0) length=snprintf(buffer,capacity,
      "[GD prior] warm=%u records=%u invalid=%u fw=%u; last 32 events, previous boot only; BEGIN without END is a lead, not proof\r\n",
      diagnosticPriorWarm,diagnosticPriorCount,diagnosticPriorInvalid,(unsigned)FIRMWARE_VER);
  else if (index<=diagnosticPriorCount) {
    const auto &r=diagnosticPrior[index-1];
    length=snprintf(buffer,capacity,"[GD prior] seq=%lu t=%lu id=%lu %s %s code=%ld elapsed=%lu detail=%lu stage=%s kind=%s\r\n",
      (unsigned long)r.sequence,(unsigned long)r.at,(unsigned long)r.id,DiagnosticName(r.event),
      r.phase==GeneratorDiagnostic::Begin?"BEGIN":r.phase==GeneratorDiagnostic::End?"END":"POINT",
      (long)r.code,(unsigned long)r.elapsed,(unsigned long)r.detail,DiagnosticStageName(r.context),
      DiagnosticRequestName(r.context>>4));
  } else return 0;
  return length>0 ? (static_cast<size_t>(length)<capacity?static_cast<size_t>(length):capacity-1):0;
}
// The boot-time eight stay as before (Crash). The rest of the retained window
// follows one event per step after boot settles, as Progress records so reboot
// evidence tables keep their Wi-Fi/recovery rows, paused for 300 ms after a
// DFPlayer call (flash writes stall the audio UART). Same labels as the boot
// replay; prior_sequence gives the order. A rejected log is retried next step.
static void DiagnosticPriorReplayStep(uint32_t now) {
  if (diagnosticPriorReplayDone || int32_t(now-diagnosticPriorReplayAt)<0) return;
  if (!BlackBoxTraceAudioQuiet(now,DiagnosticPriorQuietMs)) return;
  diagnosticPriorReplayAt=now+DiagnosticPriorReplayStepMs;
  while (diagnosticPriorNext<diagnosticPriorCount && diagnosticPriorLogged[diagnosticPriorNext]) ++diagnosticPriorNext;
  if (diagnosticPriorNext>=diagnosticPriorCount) {
    if (has1blackbox::blackbox.log(has1blackbox::EventType::Progress,diagnosticPriorLater,
        diagnosticPriorCount,diagnosticPriorWarm,diagnosticPriorTimeLost,"prior.rtc.more.end"))
      diagnosticPriorReplayDone=true;
    return;
  }
  const auto &r=diagnosticPrior[diagnosticPriorNext];
  if (!has1blackbox::blackbox.log(has1blackbox::EventType::Progress,
      r.sequence,(uint32_t(r.event)<<8)|r.phase,r.code,r.detail,"prior.rtc.event")) return;
  if (!has1blackbox::blackbox.log(has1blackbox::EventType::Progress,
      r.sequence,r.at,r.elapsed,r.id,"prior.rtc.time")) ++diagnosticPriorTimeLost;
  diagnosticPriorLogged[diagnosticPriorNext++]=true;
  ++diagnosticPriorLater;
}
void DiagnosticLoop() {
  static bool seen=false;
  static uint32_t previous=0,healthAt=0,maxGap=0;
  static GeneratorDiagnostic::RateGate gapGate;
  const uint32_t now=millis();
  DiagnosticPriorReplayStep(now);
  if (seen) {
    const uint32_t gap=uint32_t(now-previous);
    if (gap>maxGap) maxGap=gap;
    if (gap>=1000 && gapGate.allow(now,2000)) {
      DiagnosticPoint(GeneratorDiagnostic::LoopGap,0,gap);
      DiagnosticPrintf("[GD] t=%lu loop_gap=%lu ms suppressed=%lu\r\n",
        (unsigned long)now,(unsigned long)gap,(unsigned long)gapGate.skipped);
      gapGate.skipped=0;
    }
  }
  previous=now; seen=true;
  if (uint32_t(now-healthAt)<10000) return;
  healthAt=now;
  DiagnosticPrintf("[GD] t=%lu HEALTH heap=%lu min_heap=%lu largest=%lu wifi=%d rssi=%d max_loop_gap=%lu\r\n",
    (unsigned long)now,(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getMinFreeHeap(),
    (unsigned long)ESP.getMaxAllocHeap(),(int)WiFi.status(),(int)WiFi.RSSI(),(unsigned long)maxGap);
  maxGap=0;
}
// HAS2 hook is synchronous and main-task only. No URL, UID or response payload.
void DiagnosticHttpHook(const char *request,const char *phase,int32_t result,uint32_t elapsed,uint32_t detail) {
  static uint32_t id=0,started=0;
  static bool verbose=false;
  static uint8_t kind=0;
  if (!strcmp(phase,"request.begin")) {
    started=millis(); kind=DiagnosticRequestKind(request);
    id=DiagnosticBegin(GeneratorDiagnostic::Has2Http,kind,verbose);
    DiagnosticRecord(GeneratorDiagnostic::Has2Http,GeneratorDiagnostic::Point,id,0,0,0,uint8_t((kind<<4)|1));
  } else if (!strcmp(phase,"request.end")) {
    DiagnosticEnd(GeneratorDiagnostic::Has2Http,id,started,result?1:-1,detail,verbose);
    if (kind==1 && result==1 && (int)shift_machine["watchdog"]>=1) {
      const uint32_t flag=(uint32_t)(int)shift_machine["watchdog"];
      DiagnosticPoint(GeneratorDiagnostic::ServerReset,1,flag);
      DiagnosticPrintf("[GD] t=%lu server_watchdog=%lu observed; existing policy may request restart\r\n",
          (unsigned long)millis(),(unsigned long)flag);
    }
  } else {
    DiagnosticRecord(GeneratorDiagnostic::Has2Http,GeneratorDiagnostic::Point,id,result,elapsed,detail,uint8_t((kind<<4)|DiagnosticStage(phase)));
    if (verbose) DiagnosticPrintf("[GD] t=%lu id=%lu has2_http kind=%.20s phase=%.20s code=%ld elapsed=%lu detail=%lu\r\n",
      (unsigned long)millis(),(unsigned long)id,request,phase,(long)result,(unsigned long)elapsed,(unsigned long)detail);
  }
}
