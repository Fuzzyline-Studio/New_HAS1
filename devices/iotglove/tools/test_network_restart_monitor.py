"""Run the actual main recovery monitor/watchdog registration with fake time and I/O."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
GLOVE = Path(__file__).resolve().parents[1]
def functions():
    source = (GLOVE / "iotglove.cpp").read_text()
    output = []
    for name in ("armMainWatchdog", "monitorNetwork"):
        match = re.search(rf"void {name}\(\) \{{", source)
        start, cursor, depth = match.start(), match.end(), 1
        while depth:
            char = source[cursor]
            depth += (char == "{") - (char == "}")
            cursor += 1
        output.append(source[start:cursor])
    return "\n".join(output)
HARNESS = r"""
#include "network_restart.h"
#include "link_loss.h"
#include <cassert>
#include <cstring>
using namespace iotglove;
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1;
struct esp_task_wdt_config_t { unsigned timeout_ms=0,idle_core_mask=0; bool trigger_panic=false; };
void* mainWatchdog=nullptr;
unsigned watchdogAdds=0, watchdogTimeout=0;
esp_err_t esp_task_wdt_reconfigure(esp_task_wdt_config_t* c) {watchdogTimeout=c->timeout_ms;return ESP_OK;}
esp_err_t esp_task_wdt_init(esp_task_wdt_config_t*) {return ESP_OK;}
esp_err_t esp_task_wdt_add_user(const char*,void** out) {++watchdogAdds;*out=(void*)1;return ESP_OK;}
namespace has1blackbox {
enum class EventType {State,Recovery};
struct Logger { void log(EventType,int32_t,int32_t,int32_t,int32_t,const char*) {} } blackbox;
}
struct Esp { unsigned restarts=0; void restart(){++restarts;} } ESP;
NetworkRestartHistory networkRestartHistory{NetworkRestartPolicy::kMagic,0,false};
NetworkRestartPolicy networkRestartPolicy;
NetworkDiagnostics sample;
LinkLossRecord linkLossRecord{};
bool networkReady=true,stallReported=false,linkDownReported=false,ota=false;
uint32_t nowMs=0,observedLossCount=0,bootId=1,lastBatteryAdcSample=0;
int lastValidPhase=0,lastValidRole=0,lastValidDeviceState=0,restartReason=0;
uint32_t millis(){return nowMs;}
bool networkDiagnostics(NetworkDiagnostics& d){d=sample;return true;}
bool otaBusy(){return ota;}
LinkLossContext currentLinkLossContext(uint32_t){return {};}
int blackBoxAdcMv(uint16_t value,bool){return value;}
const char* phaseName(int){return "test";}
const char* roleName(int){return "test";}
const char* deviceStateName(int){return "test";}
const char* motorAgeText(uint32_t){return "test";}
template<class... T> void remoteConsoleLogf(const char*,T...){}
void blackBoxBeforeRestart(int reason,uint32_t){restartReason=reason;}
unsigned motorOffs=0;
void motorOff(){assert(!ESP.restarts);++motorOffs;} // Motor must be cut before the restart.
// PRODUCTION_FUNCTIONS
void tick(uint32_t at,bool connected=false){
  nowMs=at;sample.phase=NetworkPhase::Backoff;sample.sequence=at;
  sample.startedAtMs=sample.progressAtMs=at;sample.deadlineMs=20000;
  sample.wifiWasUp=true;sample.wifiConnected=connected;monitorNetwork();
}
void reset(unsigned attempts){
  networkRestartHistory={NetworkRestartPolicy::kMagic,attempts,false};
  networkRestartPolicy=NetworkRestartPolicy{};sample=NetworkDiagnostics{};
  linkLossRecord={};stallReported=linkDownReported=ota=false;ESP.restarts=0;
  motorOffs=0;
}
int main(){
  reset(UINT32_MAX);armMainWatchdog();
  assert(mainWatchdog && watchdogAdds==1 && watchdogTimeout==15000);
  armMainWatchdog();assert(watchdogAdds==1);
  reset(1); // An earlier restart never delays the next one.
  tick(0,true);tick(1000);tick(10999);assert(!linkDownReported && !ESP.restarts);
  tick(11000);assert(linkDownReported && ESP.restarts==1 && restartReason==3 && motorOffs==1);
  assert(networkRestartHistory.attempts==2 && networkRestartHistory.requested);
  reset(1);tick(0,true);tick(1000);tick(9000,true);tick(300000,true);assert(!ESP.restarts);
  reset(1);tick(0,true);tick(1000);ota=true;tick(300000);assert(!ESP.restarts);
  reset(1);sample.phase=NetworkPhase::Http;sample.deadlineMs=20000;sample.wifiConnected=true;
  nowMs=19999;monitorNetwork();assert(!stallReported && !ESP.restarts);
  nowMs=20000;monitorNetwork();assert(stallReported && ESP.restarts==1 && restartReason==2 && motorOffs==1);
  // The worker's outage start is stale (boot): the main loop times it itself.
  reset(0);tick(600000,true);sample.wifiDownSinceMs=0;
  tick(600100);tick(610099);assert(!ESP.restarts);
  tick(610100);assert(ESP.restarts==1 && restartReason==3);
  reset(0);tick(0);tick(179999);assert(!ESP.restarts); // Never associated this boot.
  tick(180000);assert(ESP.restarts==1);
  reset(300);tick(0,true);tick(1);tick(10001);assert(ESP.restarts==1 && linkLossRecord.restartAttempt==255);
}
"""
class MonitorTest(unittest.TestCase):
    def test_actual_monitor_and_always_armed_watchdog(self):
        with tempfile.TemporaryDirectory(prefix="glove-restart-monitor-") as work:
            source=Path(work)/"monitor.cpp"; binary=Path(work)/"monitor"
            source.write_text(HARNESS.replace("// PRODUCTION_FUNCTIONS",functions()))
            subprocess.run([os.environ.get("CXX","c++"),"-std=c++17","-Wall","-Wextra","-Werror","-pedantic",
                            "-I",str(GLOVE),str(source),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    def test_main_loop_reads_the_live_link_state(self):
        source=(GLOVE/"wifi.cpp").read_text(encoding="utf-8")
        body=source[source.index("bool networkDiagnostics(NetworkDiagnostics& out) {"):]
        body=body[:body.index("\n}\n")]
        self.assertLess(body.index("out = diagnostics;"),body.index("out.wifiConnected = linkUp.load();"))
if __name__ == "__main__":
    unittest.main()
