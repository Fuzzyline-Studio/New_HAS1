#include "blackbox.h"
#include <BlackBoxWifi.h>
#include <BlackBoxEvidence.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

static GeneratorBlackBox::BlackBoxSummary blackBoxSummary;
static uint32_t blackBoxValidAt=0,blackBoxServerAt=0;
static bool blackBoxHasValid=false;
static bool blackBoxNetworkBusy=true,blackBoxMaintenance=false;
static void BlackBoxUploadGate() {
  const char *phase=my["game_state"]|"";
  // Only recent validated device state permits upload; Unknown is fail closed.
  const bool known=blackBoxHasValid && uint32_t(millis()-blackBoxServerAt)<15000 && phase[0];
  has1blackbox::blackbox.setUploadAllowed(GeneratorBlackBox::blackBoxUploadAllowed(known,
      !strcmp(phase,"activate"),!strcmp(phase,"photo"),WiFi.status()==WL_CONNECTED,
      blackBoxNetworkBusy,blackBoxMaintenance));
}
// Plug/audio transitions (generator_trace.h). Callers only store to RAM; the
// flash log happens here, at the top of the main loop and at least 100 ms
// after the last DFPlayer call, so it never overlaps the audio UART exchange.
static GeneratorTrace::Buffer blackBoxTrace;
void BlackBoxTrace(uint8_t kind,int32_t a,int32_t b,int32_t c) {
  blackBoxTrace.add(kind,millis(),a,b,c);
}
bool BlackBoxTraceAudioQuiet(uint32_t now,uint32_t ms) { return blackBoxTrace.audioQuiet(now,ms); }
static void BlackBoxTraceFlush() {
  static const char *const labels[]={"wire.raw","wire.present","wire.ack","audio.begin","audio.end"};
  static_assert(sizeof(labels)/sizeof(labels[0])==GeneratorTrace::Count,"one label per trace kind");
  GeneratorTrace::Entry entry;
  uint32_t age,extra;
  if(!blackBoxTrace.next(millis(),entry,age,extra)) return;
  // Fourth value: event age in ms (bits 0-15, saturated) | extra << 16.
  has1blackbox::blackbox.log(has1blackbox::EventType::State,entry.a,entry.b,entry.c,
      int32_t((age>0xffffUL?0xffffUL:age) | ((extra>0x7fffUL?0x7fffUL:extra)<<16)),labels[entry.kind]);
}
void BlackBoxBegin() {
  static char version[16];snprintf(version,sizeof(version),"%d",FIRMWARE_VER);
  has1blackbox::blackbox.begin({"HAS1_generator",version,HAS1_GENERATOR_BLACKBOX_BROKER_URI,
      HAS1_GENERATOR_BLACKBOX_ENABLED!=0,HAS1_GENERATOR_BLACKBOX_MQTT_USERNAME,HAS1_GENERATOR_BLACKBOX_MQTT_PASSWORD});
  has1blackbox::blackbox.log(has1blackbox::EventType::Boot,esp_reset_reason(),FIRMWARE_VER,PARTITION_VER,0,"device.boot");
  has1blackbox::blackbox.log(has1blackbox::EventType::State,HAS1_GENERATOR_BLE_ENABLED,
      HAS1_GENERATOR_BLACKBOX_ENABLED,0,0,"build.ble.blackbox");
  WiFi.onEvent([](WiFiEvent_t event,WiFiEventInfo_t info) {
    has1blackbox::logWifiMetadata(event,info);
  });
}
void BlackBoxLoop() {
  BlackBoxTraceFlush();
  const uint32_t now=millis();BlackBoxUploadGate();
  if(!blackBoxSummary.sample(now)) return;
  has1blackbox::blackbox.log(has1blackbox::EventType::Progress,blackBoxSummary.maxGap,
      blackBoxSummary.loops,blackBoxHasValid?int32_t(now-blackBoxValidAt):-1,0,"main.valid_age");
  has1blackbox::blackbox.log(has1blackbox::EventType::Health,ESP.getFreeHeap(),ESP.getMinFreeHeap(),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),uxTaskGetStackHighWaterMark(nullptr),"memory.main");
  has1blackbox::blackbox.log(has1blackbox::EventType::WifiSummary,int(WiFi.status()),
      WiFi.status()==WL_CONNECTED?WiFi.RSSI():INT32_MIN,0,0,"wifi");
  has1blackbox::blackbox.log(has1blackbox::EventType::State,encoderValue,WireDisplayCount(),
      starterTaggerActive,ledBrightness,"encoder.wire.tagger.led");
  has1blackbox::blackbox.log(has1blackbox::EventType::Health,-1,-1,0,0,"adc.battery.unavailable");
  blackBoxSummary.emitted(now);
}
void BlackBoxNetwork(bool busy,bool maintenance,uint32_t request,int phase,uint32_t elapsed,uint32_t progressAge,uint32_t stack) {
  blackBoxNetworkBusy=busy;blackBoxMaintenance=maintenance;BlackBoxUploadGate();
  static GeneratorBlackBox::BlackBoxRateGate gate;
  if(gate.allow(millis())) {
    has1blackbox::blackbox.log(has1blackbox::EventType::Progress,request,phase,elapsed,progressAge,"network.summary");
    has1blackbox::blackbox.log(has1blackbox::EventType::Health,stack,busy,maintenance,0,"worker.stack.busy.ota");
  }
}
void BlackBoxHttp(int kind,int error,int status,uint32_t elapsed,bool stale) {
  static GeneratorBlackBox::BlackBoxRateGate gate,failureGate;
  if((error || status<200 || status>=300 || stale) && failureGate.allow(millis(),10000))
    has1blackbox::blackbox.log(has1blackbox::EventType::HttpSummary,kind,error,status,elapsed,"worker.failure");
  static uint32_t total=0,failed=0,maxMs=0;
  ++total;if(error || status<200 || status>=300 || stale) ++failed;if(elapsed>maxMs)maxMs=elapsed;
  if(!gate.allow(millis())) return;
  has1blackbox::blackbox.log(has1blackbox::EventType::HttpSummary,kind,error,status,elapsed,"worker.result");
  has1blackbox::blackbox.log(has1blackbox::EventType::HttpSummary,total,failed,maxMs,stale,"worker.counts");
  total=failed=maxMs=0;
}
void BlackBoxValidSnapshot(bool valid) {
  if(valid) { blackBoxHasValid=true;blackBoxValidAt=blackBoxServerAt=millis(); }
  static GeneratorBlackBox::BlackBoxRateGate gate;
  if(!gate.allow(millis())) return;
  has1blackbox::blackbox.log(has1blackbox::EventType::State,valid,
      blackBoxHasValid?int32_t(millis()-blackBoxValidAt):-1,0,0,"snapshot.valid");
  // Only semantic phase/state, never player UID, MAC mapping or server payload.
  has1blackbox::blackbox.log(has1blackbox::EventType::State,0,0,0,0,my["game_state"]|"unknown");
  has1blackbox::blackbox.log(has1blackbox::EventType::State,1,0,0,0,my["device_state"]|"unknown");
}
void BlackBoxPollResult(bool valid) {
  if(valid) blackBoxServerAt=millis();
  static GeneratorBlackBox::BlackBoxRateGate failureGate;
  if(!valid && failureGate.allow(millis(),10000))
    has1blackbox::blackbox.log(has1blackbox::EventType::State,0,0,0,0,"poll.invalid");
}
void BlackBoxBeforeRestart() {
  has1blackbox::blackbox.setUploadAllowed(false);
  has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,1,0,0,0,"server.restart");
  // Existing explicit restart remains immediate; no watchdog feed or new wait.
}
void BlackBoxOtaRestart(uint32_t attempts) {
  has1blackbox::blackbox.setUploadAllowed(false);
  has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,attempts,0,0,0,"ota.github.restart");
}
void BlackBoxDiagnostic(uint16_t event,uint8_t phase,uint32_t id,int32_t code,uint32_t elapsed,uint32_t detail) {
  // Sample matched operation pairs; a selected BEGIN survives even when an I/O
  // call never returns. Faults also have their own bounded gate. No strings/UIDs.
  static GeneratorDiagnostic::RateGate normal[GeneratorDiagnostic::LastEvent+1],faults[GeneratorDiagnostic::LastEvent+1];
  static uint32_t selected[GeneratorDiagnostic::LastEvent+1]={};
  static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
  if(event>GeneratorDiagnostic::LastEvent) return;
  const bool operation=event==GeneratorDiagnostic::Neo || event==GeneratorDiagnostic::AudioWait ||
      event==GeneratorDiagnostic::WireHttp || event==GeneratorDiagnostic::Has2Http;
  bool allow=false;
  portENTER_CRITICAL(&mux);
  if(operation && phase==GeneratorDiagnostic::Begin && normal[event].allow(millis(),60000)) {
    selected[event]=id;allow=true;
  }
  if(phase==GeneratorDiagnostic::End && id && selected[event]==id) {selected[event]=0;allow=true;}
  if((code<0 || elapsed>=250) && faults[event].allow(millis(),10000)) allow=true;
  portEXIT_CRITICAL(&mux);
  if(allow) {
    has1blackbox::blackbox.log(has1blackbox::EventType::Progress,
        id,(uint32_t(event)<<8)|phase,code,elapsed,"operation.sample");
    has1blackbox::blackbox.log(has1blackbox::EventType::Progress,
        id,event,phase,detail,"operation.detail");
  }
}

void BlackBoxHttpDetail(const NetworkRequest &request,const Has2BoundedTransport::Result &result,bool stale,int discard) {
  static has1blackbox::EvidenceGate gates[9];
  static uint32_t lastSuccess[9]={};
  const unsigned kind=unsigned(request.kind);
  if(kind>=9)return;
  const uint32_t now=millis();
  const bool failed=result.error!=Has2BoundedTransport::Error::None || result.status<200 || result.status>=300 || stale;
  if(gates[kind].observe(now,failed,int(result.error),int(result.phase),result.status)) {
    using has1blackbox::blackbox;using has1blackbox::EventType;
    blackbox.log(EventType::HttpSummary,request.id,kind,int(result.error),result.status,
        failed?"http.failure.kind":"http.recovered.kind");
    blackbox.log(EventType::HttpSummary,request.id,int(result.phase),result.socketError,
        result.payload.length(),"http.phase.errno.body_bytes");
    blackbox.log(EventType::HttpSummary,request.id,request.started-request.enqueued,
        result.timings.totalMs,discard,"http.queue.total.discard");
    blackbox.log(EventType::HttpSummary,request.id,result.timings.connectMs,
        result.timings.writeMs,result.timings.firstByteMs,"http.connect.write.first");
    blackbox.log(EventType::HttpSummary,request.id,result.timings.headerMs,
        result.timings.bodyMs,result.timings.closeMs,"http.header.body.close");
    blackbox.log(EventType::HttpSummary,request.id,
        lastSuccess[kind]?int32_t(now-lastSuccess[kind]):has1blackbox::Unknown,
        stale,request.generation,"http.ok_age.stale.generation");
  }
  if(!failed)lastSuccess[kind]=now;
}
void BlackBoxRecoveryState(unsigned depth,bool mailbox,bool scanPending,bool driverFault,bool maintenance) {
  static int previous=-1;
  static unsigned previousDepth=0;
  static uint32_t at=0;
  const int flags=(mailbox?1:0)|(scanPending?2:0)|(driverFault?4:0)|(maintenance?8:0);
  if((flags==previous && depth==previousDepth) || (previous!=-1 && uint32_t(millis()-at)<1000))return;
  previous=flags;previousDepth=depth;at=millis();
  has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,depth,flags,
      WiFi.status(),0,"worker.queue.gates");
}

void BlackBoxOfflineRecovery(int stage,uint32_t outageMs,uint32_t attempts,uint32_t detail) {
  // stage: 1 outage start, 2 deferred, 3 restart, 4 healthy backoff reset,
  // 5 retained prior restart. detail: delay at start, blocker bits if deferred.
  has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,
      stage,outageMs,attempts,detail,"wifi.offline.recovery");
}
