#!/usr/bin/env python3
"""Compile the production wire state/transport/parser against a fake device/server."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
include = os.environ.get("ARDUINOJSON_INCLUDE")
if not include:
    for candidate in (Path("/private/tmp/generator-absolute-deps/ArduinoJson/src"),
                      ROOT.parents[1]/".testdeps/ArduinoJson/src"):
        if (candidate/"ArduinoJson.h").is_file():
            include=str(candidate)
            break
if not include:
    raise SystemExit("Set ARDUINOJSON_INCLUDE to ArduinoJson v7.4.3/src (see tests/README.md).")

PREFIX = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "generator_wire_state.h"
#include "generator_wire_protocol.h"
static uint32_t now = 0;
static int criticalDepth = 0;
static bool pauseSampler = false, failSamplerCreation = false;
static uint32_t nextSamplerWake = 0;
static void (*fakeTaskEntry)(void*) = nullptr;
struct StaticTask_t {};
using StackType_t = uint8_t;
using TaskHandle_t = void*;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do { (void)(x); assert(++criticalDepth==1); } while(0)
#define portEXIT_CRITICAL(x) do { (void)(x); assert(--criticalDepth==0); } while(0)
#define pdMS_TO_TICKS(x) (x)
#define tskNO_AFFINITY -1
struct TaskYield {};
void vTaskDelay(uint32_t ticks) { assert(criticalDepth==0 && ticks==100); nextSamplerWake=now+ticks; throw TaskYield(); }
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void*),const char*,size_t,void*,int,StackType_t*,StaticTask_t*,int) {
 assert(criticalDepth==0);
 if(failSamplerCreation) return nullptr;
 fakeTaskEntry=entry; nextSamplerWake=now; return reinterpret_cast<void*>(1);
}
void advanceTime(uint32_t elapsed) {
 const uint32_t target=now+elapsed;
 if(fakeTaskEntry && !pauseSampler) {
   if(nextSamplerWake<now) nextSamplerWake=now; // Actual observation now; no back-filled ticks.
   while(nextSamplerWake<=target) {
     now=nextSamplerWake;
     try { fakeTaskEntry(nullptr); assert(false); } catch(const TaskYield&) {}
   }
 }
 now=target;
}
unsigned long millis() { return now; }
void delay(unsigned long value) { advanceTime(value); }
static int physical = 0, physicalMask = -1, gpioReads = 0;
#define WIRE_PIN_1 0
#define WIRE_PIN_2 1
#define WIRE_PIN_3 2
#define WIRE_PIN_4 3
#define LOW 0
#define INPUT_PULLUP 2
void pinMode(int,int) {}
static void (*gpioReadHook)() = nullptr;
int digitalRead(int pin) {
 assert(criticalDepth==0);
 if(gpioReadHook) { auto hook=gpioReadHook; gpioReadHook=nullptr; hook(); }
 ++gpioReads;
 return (physicalMask >= 0 ? (physicalMask & (1 << pin)) != 0 : pin < physical) ? LOW : 1;
}
static bool completionPending=false;
bool GeneratorNetworkCompletionPending(){return completionPending;}
static int observedProtocol=-1;
void GeneratorNetworkObserveProtocol(const GeneratorWireSnapshot &snapshot){observedProtocol=snapshot.completionProtocol;}
JsonDocument my;
bool batteryFinishDone=false, batteryFinishAudioPlayed=false, receiveMineOn=false;
int completions=0, refreshes=0, activations=0, renders=0, audio=0;
int WireCountPlugged();
void WirePollMain();
void WireObserveServerSnapshot();
void WireResetTracking();
void WaitFunc() {}
void BatteryFinish() { ++completions; }
void (*ptrCurrentMode)() = WirePollMain;
void BatteryPackSend() { assert(criticalDepth==0); ++renders; }
void Mp3PlayLargeFolder(int,int) { assert(criticalDepth==0); ++audio; }
#define MP3_WAIT_TIMEOUT_MS 2000
static bool dfPlayerReady=true, floodAudioEvents=false;
static const int DFPlayerPlayFinished=5;
static std::vector<int> dfEvents;
static size_t dfRead=0;
static int dfAvailableCalls=0;
struct FakePlayer {
 bool hasPendingInput() { return floodAudioEvents || dfRead<dfEvents.size(); }
 void resetReceiveState() {}
 bool available() { assert(criticalDepth==0); ++dfAvailableCalls; return floodAudioEvents || dfRead<dfEvents.size(); }
 int readType() { assert(criticalDepth==0); return floodAudioEvents ? 1 : dfEvents.at(dfRead++); }
} myDFPlayer;
void SyncBatteryPackCur() {}
void DataChanged() { WireObserveServerSnapshot(); }
void ActivateFunc() { ++activations; ptrCurrentMode=WirePollMain; }
struct Logger {
 void println(const char*) { assert(criticalDepth==0); }
 void printf(const char*,...) { assert(criticalDepth==0); }
} Serial;
static GeneratorWireSnapshot serverState;
static const char EPOCH1[]="11111111-1111-1111-1111-111111111111";
static const char EPOCH2[]="22222222-2222-2222-2222-222222222222";
struct WifiLibrary {
 std::string GetMacAddress() { return "AA:BB:CC:DD:EE:FF"; }
 void ReceiveMine() {
   ++refreshes;
   my["device_name"]=serverState.device;
   my["game_state"]=serverState.gameState;
   my["device_state"]=serverState.deviceState;
   my["battery_pack"]=serverState.count;
   my["max_battery_pack"]=serverState.maximum;
 }
} has2wifi;
static uint32_t testNetworkGeneration=1;
uint32_t GeneratorNetworkGeneration() { return testNetworkGeneration; }
void GeneratorNetworkInvalidateGame() { ++testNetworkGeneration; }
void GeneratorNetworkRefresh() { has2wifi.ReceiveMine(); DataChanged(); }
void GeneratorNetworkRefreshCycle() { GeneratorNetworkRefresh(); }
static int readRequests=0;
struct Write { int count; uint32_t revision; std::string epoch; };
static std::vector<Write> writes;
enum SendMode { Normal, FailNoApply, FailAfterApply, WrongEpoch, WrongRevision, WrongCount, Conflict };
static SendMode nextSend=Normal;
static uint32_t httpDelay=0;
static bool removeDuringHttp=false, resetDuringHttp=false;
GeneratorWireResult WireHttpRequest(bool set,const char *name,int count,const char *epoch,uint32_t revision,GeneratorWireSnapshot &reply) {
 assert(criticalDepth==0);
 advanceTime(httpDelay/2);
 if(set && removeDuringHttp) { physical=2; removeDuringHttp=false; }
 if(set && resetDuringHttp) { WireResetTracking(); resetDuringHttp=false; }
 advanceTime(httpDelay-httpDelay/2);
 assert(strcmp(name,serverState.device)==0);
 if(!set) { ++readRequests; reply=serverState; return GeneratorWireResult::Ok; }
 writes.push_back({count,revision,epoch});
 SendMode mode=nextSend; nextSend=Normal;
 if(mode==FailNoApply) return GeneratorWireResult::Failed;
 if(mode==Conflict || strcmp(epoch,serverState.epoch)!=0 || revision!=serverState.revision) {
   reply=serverState; return GeneratorWireResult::Conflict;
 }
 serverState.count=count; ++serverState.revision; reply=serverState;
 if(mode==FailAfterApply) return GeneratorWireResult::Failed;
 if(mode==WrongEpoch) strcpy(reply.epoch,EPOCH2);
 if(mode==WrongRevision) reply.revision=revision;
 if(mode==WrongCount) reply.count=count==0?1:0;
 return GeneratorWireResult::Ok;
}

'''

HTTP = r'''
struct String : std::string {
 using std::string::string;
 String(const std::string &text):std::string(text) {}
 String(int number):std::string(std::to_string(number)) {}
 String(unsigned number):std::string(std::to_string(number)) {}
};
#define HAS2_BT_HOST 1
#include "has2_bounded_transport.h"
enum class NetworkKind { Wire };
struct NetworkRequest { uint32_t generation=0,token=0; };
static NetworkRequest queuedRequest;
static void (*queuedCallback)(const NetworkRequest&,const Has2BoundedTransport::Result&,bool)=nullptr;
static bool rejectQueue=false;
static unsigned queuedCount=0;
static uint32_t NetworkSubmit(NetworkKind,const String&,bool,uint32_t generation,uint32_t token,
    void (*callback)(const NetworkRequest&,const Has2BoundedTransport::Result&,bool),int=-1) {
  if(rejectQueue)return 0;
  assert(!queuedCallback);
  queuedRequest.generation=generation;queuedRequest.token=token;queuedCallback=callback;
  return ++queuedCount;
}
static bool NetworkReplaceQueued(uint32_t,const String&,uint32_t,uint32_t,int) { return false; }
static std::string httpBody;
static int httpStatus=200;
static bool httpStale=false,httpFailed=false;
static void deliver() {
  assert(queuedCallback);
  auto callback=queuedCallback;queuedCallback=nullptr;
  Has2BoundedTransport::Result result;
  result.status=httpStatus;result.payload=httpBody;
  if(httpFailed)result.error=Has2BoundedTransport::Error::Deadline;
  callback(queuedRequest,result,httpStale);
}
'''

ASYNC_WRAPPER = r'''
static GeneratorWireResult ActualWireHttpRequest(bool set,const char *device,int count,
    const char *epoch,uint32_t revision,GeneratorWireSnapshot &snapshot) {
  const auto first=QueuedWireHttpRequest(set,device,count,epoch,revision,snapshot);
  if(first!=GeneratorWireResult::Pending)return first;
  deliver();
  return QueuedWireHttpRequest(set,device,count,epoch,revision,snapshot);
}
'''

TESTS = r'''
static void reset(int count=0,int maximum=3) {
 now=0; physical=count; physicalMask=-1; gpioReads=0; my.clear();
 fakeTaskEntry=nullptr; pauseSampler=failSamplerCreation=false; criticalDepth=0; nextSamplerWake=0;
 serverState=GeneratorWireSnapshot();
 strcpy(serverState.device,"LG"); strcpy(serverState.epoch,EPOCH1);
 strcpy(serverState.gameState,"activate"); strcpy(serverState.deviceState,"activate");
 serverState.count=count; serverState.maximum=maximum; serverState.revision=7;
 has2wifi.ReceiveMine(); refreshes=0;
 wireState=GeneratorWireState(); wireEnabled=false; wireContextValid=false;
 wireRetryPending=false; wireFenceNeeded=true; wireServerCompleted=false; wireLastAttempt=0; wireContext=GeneratorWireSnapshot();
 memset(wireObservedGame,0,sizeof(wireObservedGame));
 memset(wireObservedDevice,0,sizeof(wireObservedDevice)); memset(wireObservedName,0,sizeof(wireObservedName));
 writes.clear(); readRequests=activations=renders=audio=completions=0;
 nextSend=Normal; httpDelay=0; removeDuringHttp=resetDuringHttp=false; batteryFinishDone=batteryFinishAudioPlayed=receiveMineOn=false;
 ptrCurrentMode=WirePollMain; wireSamplerTask=nullptr; wireSamplingEnabled=false; wireLastPresented=-1;
 gpioReadHook=nullptr; dfPlayerReady=true; floodAudioEvents=false; dfEvents.clear(); dfRead=0; dfAvailableCalls=0;
 WireInit(); WireObserveServerSnapshot();
}
static void tick(uint32_t elapsed=100) { advanceTime(elapsed); WireServiceLoop(); }
static void run(unsigned milliseconds) {
 for(unsigned i=0;i<milliseconds;i+=100) { tick(); }
}
static void settle(int count) { physical=count; run(3200); }
static std::string snapshotJson() {
 JsonDocument json; json["success"]=true; json["device_name"]="LG";
 json["battery_pack"]=3; json["max_battery_pack"]=3;
 json["game_state"]="activate"; json["device_state"]="activate";
 json["epoch"]=EPOCH1; json["revision"]=8;
 std::string result; serializeJson(json,result); return result;
}
int main() {
 reset(4,4);completionPending=true;run(1400);
 assert(wireState.qualified(millis())&&writes.empty()&&readRequests==0);
 completionPending=false;run(500);assert(wireState.synced()&&writes.size()==1);
 puts("PASS durable completion blocks boot wire fencing while input sampling continues; cleared intent resumes CAS");
 reset(); run(1200); assert(wireState.synced() && writes.size()==1 && readRequests==1);
 assert(writes[0].count==0 && serverState.revision==8); // startup fences an earlier boot
 writes.clear(); readRequests=0;
 for(unsigned duration=100;duration<=700;duration+=100) {
   physical=2; run(duration); physical=0; run(1200);
 }
 assert(writes.empty() && readRequests==0);
 puts("PASS 100..700 ms round trips produce no writes; initial boot is fenced");

 for(int value=0;value<=4;++value) {
   reset(); run(1200); writes.clear(); readRequests=0;
   physical=value; run(700); assert(writes.empty()); run(2400);
   assert(wireState.stable()==value && wireState.synced());
   assert(writes.size()==(value?1u:0u));
   size_t confirmed=writes.size(); run(6000); assert(writes.size()==confirmed && readRequests==0);
 }
 puts("PASS 0..4 absolute counts, one stable update, no repeated confirmed-value requests");
 {
  reset(); run(1200);
  const unsigned raw=blackBoxTestTraces[GeneratorTrace::WireRaw];
  const unsigned present=blackBoxTestTraces[GeneratorTrace::WirePresent];
  const unsigned ack=blackBoxTestTraces[GeneratorTrace::WireAck];
  physical=1; run(3000);
  assert(blackBoxTestTraces[GeneratorTrace::WireRaw]>raw);
  assert(blackBoxTestTraces[GeneratorTrace::WirePresent]==present+1);
  assert(blackBoxTestTraces[GeneratorTrace::WireAck]>ack && wireState.synced());
  puts("PASS contact, presentation and acknowledgement reach the flash trace outside every lock");
 }

 reset(4,3); run(1200); assert(WireReadyForCompletion());
 writes.clear(); settle(3); assert(WireReadyForCompletion() && serverState.count==3);
 physical=2; tick(); assert(WireReadyForCompletion()); run(200); assert(!WireReadyForCompletion());
 run(3200); assert(serverState.count==2 && !WireReadyForCompletion());
 assert(writes.size()==2 && writes[0].count==3 && writes[1].count==2);
 puts("PASS LG physical 4->3->2 is not clamped to maximum 3 and removal blocks completion");

 reset(); run(1200); writes.clear(); physical=3; nextSend=FailAfterApply;
 run(2100); assert(writes.size()==1 && !wireState.acknowledged());
 const uint32_t failedAt=wireLastAttempt;
 while(now-failedAt<1900) { tick(); }
 assert(writes.size()==1);
 run(1500); assert(writes.size()==2 && writes.back().count==3 && wireState.synced());
 assert(wireLastAttempt-failedAt>=2000 && writes[1].revision>writes[0].revision);
 puts("PASS lost ACK retries unchanged physical count; matching GET still receives CAS fence");

 reset(); run(1200); writes.clear(); physical=3; nextSend=FailNoApply; run(2100);
 assert(writes.size()==1); physical=2; run(4000);
 assert(writes.size()==2 && writes.back().count==2 && serverState.count==2);
 const Write old=writes.front(); assert(old.revision<serverState.revision);
 puts("PASS retry uses latest stable count; previous request revision is fenced");

 for(SendMode bad : {WrongEpoch,WrongRevision,WrongCount,Conflict}) {
   reset(); run(1200); physical=3; nextSend=bad; run(2100);
   assert(!wireState.acknowledged() && !WireReadyForCompletion());
 }
 puts("PASS wrong identity context/count/revision and conflict cannot acknowledge completion");

 reset(3); run(1200); assert(WireReadyForCompletion());
 pauseSampler=true; tick(10000); physical=0;
 const int readsBefore=gpioReads;
 for(int i=0;i<5;++i) { assert(WireReadyForCompletion()); }
 assert(gpioReads==readsBefore); // Completion cannot add a raw veto or an extra vote.
 pauseSampler=false; run(1200); assert(!WireReadyForCompletion() && serverState.count==0);
 puts("PASS paused qualified sampler retains verdict; completion reads no GPIO and observed removal uses votes");

 reset(3); advanceTime(0); pauseSampler=true;
 tick(10000); assert(!WireCanProgress() && writes.empty() && WireSnapshot().stable()==-1);
 pauseSampler=false; advanceTime(0); WireServiceLoop();
 assert(!WireCanProgress() && writes.empty());
 run(800); assert(WireReadyForCompletion() && writes.size()==1);
 puts("PASS long gap cannot replace ten real samples; no age expiry or synthetic catch-up votes");

 reset(3); pauseSampler=true; WireSamplerTick();
 for(int sample=1;sample<=9;++sample) {
   advanceTime(300); WireSamplerTick(); WireServiceLoop();
   assert(WireReadyForCompletion()==(sample==9));
 }
 assert(writes.size()==1 && wireState.synced());
 puts("PASS repeated 300 ms actual sampling gaps qualify after ten actual observations");

 for(int connectedVotes : {7,8,9}) {
   GeneratorWireState vote;
   vote.sampleMask(0,0); // startup sample rolls out; last ten determine verdict
   for(int sample=1;sample<=10;++sample)
     vote.sampleMask(sample*100, sample<=connectedVotes ? 0x0f : 0);
   assert(vote.qualified(1000)==(connectedVotes>=8));
   if(connectedVotes>=8) {
     assert(vote.stable()==4 && vote.rawMask()==0);
     assert(vote.acknowledge(4,vote.generation()) && vote.ready(1000,4));
   }
 }
 puts("PASS exact 8/10 and 9/10 accept all pins despite final raw mismatch; 7/10 remains ambiguous");

 for(int disconnectedVotes : {7,8,9}) {
   GeneratorWireState vote;
   vote.sampleMask(0,0x0f);
   for(int sample=1;sample<=10;++sample)
     vote.sampleMask(sample*100, sample<=disconnectedVotes ? 0 : 0x0f);
   assert(vote.qualified(1000)==(disconnectedVotes>=8));
   if(disconnectedVotes>=8) assert(vote.stable()==0 && vote.rawMask()==0x0f);
 }
 puts("PASS disconnection uses the same symmetric 8/10 threshold");

 {
   GeneratorWireState vote;
   for(int sample=0;sample<=10;++sample) vote.sampleMask(sample*100, sample%2 ? 0x01 : 0x02);
   assert(!vote.qualified(1000) && vote.stable()==-1);
   vote.reset(); vote.sampleMask(0,0x0f);
   for(int sample=1;sample<=10;++sample) {
     const uint8_t mask=sample<=8 ? uint8_t(0x0f ^ (1u << ((sample-1)/2))) : 0x0f;
     vote.sampleMask(sample*100,mask);
   }
   assert(vote.qualified(1000) && vote.stable()==4);
 }
 puts("PASS equal raw totals can be ambiguous per pin; staggered per-pin noise still qualifies four");

 reset(0); run(1200); writes.clear(); physical=3;
 run(700); assert(!WireCanProgress() && wireState.stable()==0 && writes.empty());
 tick(); assert(WireReadyForCompletion() && writes.size()==1 && wireState.stable()==3);
 physical=0; run(200); assert(WireReadyForCompletion());
 tick(); assert(!WireCanProgress() && wireState.stable()==3 && writes.size()==1);
 run(400); assert(!WireCanProgress() && writes.size()==1);
 tick(); assert(wireState.qualified(now) && wireState.stable()==0 && writes.size()==2 && !WireReadyForCompletion());
 puts("PASS sliding rise/fall needs eight new votes; ambiguous windows retain UI count without publication or completion");

 {
   GeneratorWireState rollover;
   const uint32_t start=UINT32_MAX-500;
   for(uint32_t sample=0;sample<10;++sample) rollover.sampleMask(start+sample*100u,0x07);
   assert(!rollover.qualified(start+900u));
   rollover.sampleMask(start+1000u,0x07); assert(rollover.qualified(start+1000u));
   assert(rollover.acknowledge(3,rollover.generation()) && rollover.ready(start+10000u,3));
   const uint32_t generation=rollover.generation(); rollover.reset();
   assert(!rollover.acknowledge(3,generation) && !rollover.qualified(start+10000u));
 }
 puts("PASS millis rollover, initial one-second wait, no age expiry and reset clearing vote history");

 reset(3); wireSamplerTask=nullptr; fakeTaskEntry=nullptr; failSamplerCreation=true;
 WireInit(); run(3000); assert(!WireCanProgress() && writes.empty());
 puts("PASS sampler creation failure cannot complete or publish raw inputs");

 reset(3); gpioReadHook=WireResetTracking;
 advanceTime(0); assert(WireSnapshot().stable()==-1 && !WireCanProgress());
 tick(); assert(WireSnapshot().rawMask()==0x07 && !WireCanProgress());
 run(900); assert(!WireCanProgress()); tick(); assert(WireReadyForCompletion());
 puts("PASS GPIO read overlapping reset discards the previous generation observation");

 reset(); run(1200); writes.clear();
 physical=2; advanceTime(1100); physical=4; advanceTime(1100);
 WireServiceLoop(); assert(writes.size()==1 && writes[0].count==4 && WireReadyForCompletion());
 puts("PASS main stall consumes only latest stable count without replaying intermediate updates");

 reset(3); run(1200); dfEvents={DFPlayerPlayFinished,1};
 Mp3BatteryStart(); const int playedBefore=audio;
 assert(!Mp3BatteryFinished() && audio==playedBefore+1 && dfRead==2);
 physical=2; advanceTime(1100); assert(WireSnapshot().stable()==2 && !WireReadyForCompletion());
 assert(!Mp3BatteryFinished()); dfEvents.push_back(1); assert(!Mp3BatteryFinished());
 dfEvents.push_back(DFPlayerPlayFinished); assert(Mp3BatteryFinished());
 assert(audio==playedBefore+1);
 puts("PASS actual audio phase drains queued finish and sampler keeps observing while audio is pending");

 Mp3BatteryStart(); assert(!Mp3BatteryFinished());
 advanceTime(1999); assert(!Mp3BatteryFinished()); advanceTime(1); assert(Mp3BatteryFinished());
 Mp3BatteryStart(); assert(!Mp3BatteryFinished()); WireResetTracking();
 assert(batteryAudioPhase==BatteryAudioPhase::Idle && Mp3BatteryFinished());
 dfPlayerReady=false; Mp3BatteryStart(); assert(Mp3BatteryFinished());
 dfPlayerReady=true; floodAudioEvents=true; Mp3BatteryStart(); dfAvailableCalls=0;
 assert(!Mp3BatteryFinished() && dfAvailableCalls==8);
 advanceTime(2000); assert(Mp3BatteryFinished()); floodAudioEvents=false;
 puts("PASS actual audio timeout, round cancellation, unavailable player and bounded event flood");

 reset(3); httpDelay=500;
 while(!WireCanProgress() && now<4000) { tick(); }
 assert(now==2000 && WireReadyForCompletion());
 const uint32_t acknowledgedAt=wireLastAttempt;
 physical=4; while(serverState.count!=4 && now<5000) { tick(); }
 assert(wireLastAttempt-acknowledgedAt==1300 && WireReadyForCompletion());
 puts("PASS latency: stable at 1000 ms, GET/SET 500 ms each -> ready at 2000 ms; next stable send has no 2-second gate");
 physical=3; removeDuringHttp=true;
 while(writes.back().count!=3 && now<8000) { tick(); }
 assert(!WireReadyForCompletion()); run(2500); assert(serverState.count==2 && !WireReadyForCompletion());
 puts("PASS sampler observes removal during synchronous HTTP and latest count replaces old ACK");

 reset(3); httpDelay=500; resetDuringHttp=true;
 while(writes.empty() && now<4000) { tick(); }
 assert(!wireState.acknowledged() && wireRetryPending);
 puts("PASS reset during HTTP rejects an old generation ACK");

 reset(3); run(1200); assert(WireReadyForCompletion());
 strcpy(serverState.gameState,"setting"); has2wifi.ReceiveMine(); DataChanged();
 assert(!wireEnabled && !WireCanProgress() && !wireState.acknowledged());
 strcpy(serverState.epoch,EPOCH2); strcpy(serverState.gameState,"activate"); serverState.count=0;
 has2wifi.ReceiveMine(); DataChanged(); run(900); assert(!WireCanProgress()); run(4500);
 assert(wireState.synced() && serverState.count==3);
 puts("PASS setting/new round cancels old qualification and acknowledgement");

 reset(3); run(1200); batteryFinishDone=true; batteryFinishAudioPlayed=true;
 strcpy(serverState.deviceState,"battery_max"); ++serverState.revision;
 has2wifi.ReceiveMine(); DataChanged(); assert(!wireContextValid && batteryFinishDone && batteryFinishAudioPlayed);
 size_t before=writes.size(); run(2500);
 assert(wireContextValid && writes.size()==before && batteryFinishDone && batteryFinishAudioPlayed);
 puts("PASS normal battery_max echo refreshes revision without redoing writes or audio");

 for(const char *completed : {"repaired","repaired_all"}) {
   reset(3); strcpy(serverState.deviceState,completed); has2wifi.ReceiveMine(); ptrCurrentMode=WaitFunc;
   run(1200); strcpy(serverState.epoch,EPOCH2); serverState.revision=1;
   physical=2; run(4500);
   assert(activations==0 && ptrCurrentMode==WaitFunc && std::string(my["device_state"].as<const char*>())==completed);
 }
 puts("PASS new epoch never reactivates repaired/repaired_all generators");

 reset(3); serverState.count=9; has2wifi.ReceiveMine(); run(1200);
 assert(serverState.count==3 && wireState.synced());
 puts("PASS legacy out-of-range accumulated count is corrected to physical absolute count");
 reset(3); run(1200); serverState.revision=0; strcpy(serverState.device,"BG");
 has2wifi.ReceiveMine(); DataChanged(); run(3500);
 assert(wireState.synced() && strcmp(wireContext.device,"BG")==0 && serverState.count==3);
 puts("PASS new device identity does not inherit the previous device revision");

 reset(1); run(1200); assert(!WireCanProgress());
 strcpy(serverState.deviceState,"battery_max"); ++serverState.revision;
 has2wifi.ReceiveMine(); DataChanged(); run(1500); assert(!WireCanProgress());
 WireAcceptServerCompletion(); assert(WireServerCompleted() && WireCanProgress() && WireReadyForCompletion());
 run(3000); assert(WireReadyForCompletion() && serverState.count==1); // physical count still reported
 strcpy(serverState.deviceState,"tagger"); has2wifi.ReceiveMine(); DataChanged(); assert(WireCanProgress());
 strcpy(serverState.deviceState,"activate"); has2wifi.ReceiveMine(); DataChanged();
 assert(!WireServerCompleted() && !WireCanProgress());
 reset(1); run(1200); strcpy(serverState.deviceState,"battery_max"); has2wifi.ReceiveMine(); DataChanged();
 WireAcceptServerCompletion(); strcpy(serverState.gameState,"setting"); has2wifi.ReceiveMine(); DataChanged();
 assert(!WireServerCompleted() && !WireCanProgress());
 puts("PASS server-declared battery_max bypasses the wire gate only until the next charge cycle");


 // Actual JSON parser and asynchronous wire-result handling.
 reset(); GeneratorWireSnapshot parsed; JsonDocument json;
 std::string valid=snapshotJson(); deserializeJson(json,valid);
 assert(WireParseSnapshot(json,"LG",parsed));
 for(const char *field : {"battery_pack","max_battery_pack","revision"}) {
   deserializeJson(json,valid); json[field]="3"; assert(!WireParseSnapshot(json,"LG",parsed));
   deserializeJson(json,valid); json[field]=true; assert(!WireParseSnapshot(json,"LG",parsed));
   deserializeJson(json,valid); json[field]=3.5; assert(!WireParseSnapshot(json,"LG",parsed));
 }
 deserializeJson(json,valid); json["battery_pack"]=-9; assert(WireParseSnapshot(json,"LG",parsed));
 json["battery_pack"]=int64_t(2147483648LL); assert(!WireParseSnapshot(json,"LG",parsed));
 deserializeJson(json,valid); json["revision"]=uint64_t(4294967296ULL); assert(!WireParseSnapshot(json,"LG",parsed));
 deserializeJson(json,valid); json["epoch"]="old"; assert(!WireParseSnapshot(json,"LG",parsed));
 deserializeJson(json,valid); json["device_name"]="BG"; assert(!WireParseSnapshot(json,"LG",parsed));
 httpBody=valid; httpStatus=200;
 assert(ActualWireHttpRequest(true,"LG",3,EPOCH1,7,parsed)==GeneratorWireResult::Ok);
 assert(ActualWireHttpRequest(true,"LG",2,EPOCH1,7,parsed)==GeneratorWireResult::Failed);
 assert(ActualWireHttpRequest(true,"LG",3,EPOCH1,8,parsed)==GeneratorWireResult::Failed);
 deserializeJson(json,valid); json["success"]=false; json["error"]="CONFLICT";
 httpBody.clear(); serializeJson(json,httpBody); httpStatus=409;
 assert(ActualWireHttpRequest(true,"LG",3,EPOCH1,7,parsed)==GeneratorWireResult::Conflict);
 httpStatus=200; assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 puts("PASS production async wire strict JSON/ACK parser and conflict handling");
 deserializeJson(json,valid);json["device_name"]="BG";
 httpBody.clear();serializeJson(json,httpBody);
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 httpBody=valid;httpFailed=true;
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 httpFailed=false;httpStale=true;
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 httpStale=false;httpStatus=500;
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 httpStatus=200;httpBody="invalid JSON";
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 puts("PASS wrong device, expired transport, stale result, HTTP error, malformed JSON rejected");
 httpBody=valid;
 const auto beforeQueue=queuedCount;
 assert(QueuedWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Pending);
 for(int i=0;i<100;++i)assert(QueuedWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Pending);
 assert(queuedCount==beforeQueue+1);
 deliver();
 assert(QueuedWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Ok);
 assert(QueuedWireHttpRequest(true,"LG",3,EPOCH1,7,parsed)==GeneratorWireResult::Pending);
 deliver();
 assert(QueuedWireHttpRequest(true,"LG",2,EPOCH1,7,parsed)==GeneratorWireResult::Failed);
 assert(QueuedWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Pending);
 ++testNetworkGeneration;deliver();
 assert(QueuedWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 rejectQueue=true;
 assert(QueuedWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 puts("PASS one wire request in flight, changed desired value/game generation and full queue");
 // Connect/write/header/body/cleanup fault injection is covered by the actual
 // shared transport suite; this harness tests its asynchronous application seam.
 return 0;
}

'''
with tempfile.TemporaryDirectory(prefix="generator-wire-test-") as directory:
    temp = Path(directory)
    (temp/"HTTPClient.h").write_text("// Fake HTTPClient is defined by the harness.\n")
    audio_source = (ROOT/"dfplayer.ino").read_text().split('enum class BatteryAudioPhase',1)[1]
    source = PREFIX + '\n#define DIAGNOSTIC_TEST_CHECK assert(criticalDepth==0)\n#include "tests/diagnostic_test_stubs.h"\n' + '\nenum class BatteryAudioPhase'  + audio_source + '\n' + (ROOT/"wire.ino").read_text() + '\n' + HTTP
    source += '\n#define WireHttpRequest QueuedWireHttpRequest\n' + (ROOT/"wire_http.ino").read_text()
    source += '\n#undef WireHttpRequest\n' + ASYNC_WRAPPER + TESTS
    (temp/"test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX","c++"),"-std=c++11","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-fno-omit-frame-pointer",
                    "-I",str(ROOT),"-I",str(ROOT.parents[1]/"libraries/HAS2_Wifi"),"-I",include,"-I",str(temp),str(temp/"test.cpp"),"-o",str(temp/"test")],check=True)
    subprocess.run([str(temp/"test")],check=True)

COMPLETION_PREFIX = r'''
#include <cassert>
#include <string>
#include <map>
#include <iostream>
#define BREADCRUMB(x) ((void)0)
struct String:std::string { using std::string::string; };
struct Value { std::string text; Value& operator=(const char* value) {text=value;return *this;} operator const char*() const {return text.c_str();} };
struct Document { std::map<std::string,Value> values; Value& operator[](const char* key){return values[key];} } my;
struct Logger { void println(const char*) {} } Serial;
static bool ready=true, removeDuringAudio=false, removeDuringSend=false, audioFinished=true;
bool WireReadyForCompletion() { return ready; }
static bool serverCompleted=false;
bool WireServerCompleted() { return serverCompleted; }
static int accepted=0;
void WireAcceptServerCompletion() { ++accepted; serverCompleted=true; }
void TaggerReset() {}
void TaggerEnter() {}
void BatteryPackSend() {}
void BlinkTimerStart(int,int) {}
void RfidLoopMain() {}
void WirePollMain() {}
void WireResetTracking() {}
void StartFinish();
enum {MAINPN532};
struct Reader { void SAMConfig() {} } nfc[1];
bool batteryFinishDone=false,batteryFinishAudioPlayed=false,receiveMineOn=false,starterRfidNeedsValidation=false;
static int played=0,sent=0,received=0,detached=0,started=0;
static bool completionConfirmed=true;
static std::string lastCompletion;
bool GeneratorNetworkComplete(const char *state) {
 if(lastCompletion!=state){lastCompletion=state;++sent;if(removeDuringSend)ready=false;}
 return completionConfirmed;
}
long encoderValue=1000;
int displayedGaugeNeoCnt=-1,gameTimerCnt=0,gameTimerId=0,blinkTimerId=0,gameTime=400;
void GameTimerFunc() {}
struct Timer {
 void deleteTimer(int) {}
 int setInterval(int,void(*)()) {++started;return 1;}
 bool isEnabled(int) {return true;}
} GameTimer,BlinkTimer;
void WaitFunc() {}
void StarterActivate() {}
void BatteryFinish();
void (*ptrCurrentMode)()=BatteryFinish;
void (*ptrRfidMode)()=WaitFunc;
void EncoderDetach() {++detached;}
int StarterGaugeCnt() {return 1;}
void EncoderNeopixelOn(int) {}
enum {GAUGE,STARTER,DEVICESTATE,CIRCUIT,BLUE,YELLOW,GREEN};
int color[7][3];
void NeoLightColor(int,int*) {}
void AllNeoOn(int) {}
void LeftGenerator() {}
void Mp3PlayLargeFolder(int,int) {}
void Mp3PlayLargeFolderAndWait(int,int) {++played;if(removeDuringAudio)ready=false;}
void Mp3BatteryStart() {++played;if(removeDuringAudio)ready=false;}
bool Mp3BatteryFinished() {return audioFinished;}
struct Wifi {
 void Send(String,const char*,const char*) {++sent;if(removeDuringSend)ready=false;}
 void ReceiveMine() {++received;}
} has2wifi;
'''
COMPLETION_TESTS = r'''
int main() {
 my["device_name"]="LG";my["device_state"]="activate";
 ready=false;BatteryFinish();assert(sent==0&&played==0&&!batteryFinishDone);
 ready=true;audioFinished=false;BatteryFinish();
 assert(played==1&&sent==0&&!batteryFinishDone);
 BatteryFinish();assert(played==1&&sent==0);
 audioFinished=true;ready=false;BatteryFinish();
 assert(played==1&&sent==0&&batteryFinishAudioPlayed&&!batteryFinishDone);
 BatteryFinish();assert(played==1&&sent==0);
 ready=true;removeDuringAudio=false;removeDuringSend=true;BatteryFinish();
 assert(played==1&&sent==1&&batteryFinishDone&&ptrCurrentMode==BatteryFinish);
 ready=true;removeDuringSend=false;BatteryFinish();
 assert(played==1&&sent==1&&ptrCurrentMode==StarterActivate);
 std::cout<<"PASS actual BatteryFinish checks again after audio and HTTP; announcement/send are not repeated\n";
 sent=0;ready=false;StartFinish();assert(sent==0&&ptrCurrentMode==StarterActivate);
 ready=true;removeDuringSend=true;StartFinish();
 assert(sent==1&&received==0&&ptrCurrentMode==WaitFunc);
 std::cout<<"PASS actual StartFinish blocks unconfirmed inputs and does not undo an already-sent repair\n";
 batteryFinishDone=batteryFinishAudioPlayed=receiveMineOn=false;played=sent=0;
 ready=true;audioFinished=true;serverCompleted=true;ptrCurrentMode=BatteryFinish;BatteryFinish();
 assert(played==1&&sent==0&&!receiveMineOn&&batteryFinishDone&&ptrCurrentMode==StarterActivate);
 std::cout<<"PASS server-declared battery_max announces locally without sending battery_max again\n";
 // A queued completion must not advance gameplay before acknowledgement.
 batteryFinishDone=batteryFinishAudioPlayed=receiveMineOn=false;
 serverCompleted=false;ready=true;audioFinished=true;removeDuringSend=false;
 completionConfirmed=false;lastCompletion.clear();played=sent=0;
 ptrCurrentMode=BatteryFinish;
 BatteryFinish();assert(sent==1&&played==1&&!batteryFinishDone&&ptrCurrentMode==BatteryFinish);
 for(int i=0;i<10;++i)BatteryFinish();
 assert(sent==1&&played==1&&!batteryFinishDone);
 completionConfirmed=true;BatteryFinish();assert(batteryFinishDone&&ptrCurrentMode==StarterActivate);
 completionConfirmed=false;lastCompletion.clear();sent=0;
 StartFinish();assert(sent==1&&ptrCurrentMode==StarterActivate);
 for(int i=0;i<10;++i) { StartFinish(); }
 assert(sent==1&&ptrCurrentMode==StarterActivate);
 completionConfirmed=true;StartFinish();assert(ptrCurrentMode==WaitFunc);
 std::cout<<"PASS queued battery/repair completion waits for confirmation without replaying sound or mutation\n";
 for(const char *state : {"battery_max","starter_finish"}) {
   my["device_state"]=state;
   accepted=0;serverCompleted=false;batteryFinishDone=true;ActivateFunc();assert(accepted==0);
   batteryFinishDone=false;ActivateFunc();assert(accepted==1);
 }
 my["device_state"]="activate";accepted=0;batteryFinishDone=false;ActivateFunc();
 assert(accepted==0&&ptrCurrentMode==WirePollMain);
 std::cout<<"PASS actual ActivateFunc trusts server battery_max/starter_finish only for cycles not completed locally\n";
}
'''

def production_function(file, name):
    source = (ROOT/file).read_text()
    start = source.index('void '+name+'(')
    opening = source.index('{',start)
    depth, end = 1, opening+1
    while depth:
        depth += (source[end]=='{') - (source[end]=='}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix="generator-wire-completion-") as directory:
    temp=Path(directory)
    source=COMPLETION_PREFIX+'\n'+production_function('rfid.ino','BatteryFinish')+'\n'+production_function('rfid.ino','StartFinish')+'\n'+production_function('wifi.ino','ActivateFunc')+'\n'+COMPLETION_TESTS
    (temp/'test.cpp').write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(temp/'test.cpp'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test')],check=True)
