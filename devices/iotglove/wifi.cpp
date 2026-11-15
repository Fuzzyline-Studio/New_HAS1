#include "wifi_client.h"
#include "blackbox.h"
#include <BlackBoxWifi.h>
#include <BlackBoxEvidence.h>
#include "network_policy.h"
#include "chip_report.h"
#include "feedback_config.h"
#include "library_and_pin.h"
#include "secrets.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HAS2_Wifi.h>
#include <SecureOTA.h>
#include <IoTGloveOta.h>
#include <IoTGloveOtaClient.h>
#include "telnet.h"
#include <WiFi.h>
#include <atomic>
#include <errno.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace iotglove {
namespace {
constexpr char kRelease[] = "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/iotglove/";
constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kRetryMs = 5000;
HAS2_Wifi wifi;
QueueHandle_t snapshots, commands, results, locations, batteries, otaRequests;
std::atomic<bool> forceSnapshot{false}, commandBusy{false};
// Set by the main loop while the button's Found pattern plays; no new requests start.
std::atomic<uint32_t> quietUntil{0};
// 2 means no initial sensor sample yet; never retain an edge queue.
std::atomic<uint8_t> physicalChip{2};
ChipReportPolicy chipReport;
std::atomic<NetworkOtaStatus> otaState{NetworkOtaStatus::Idle};
struct Location { char room[32]; };
struct Battery { float volts; uint32_t sampledAt; };
struct OtaRequest { uint32_t targetVersion = 0; char sourceCommand[40] = {}; };
int version, partitionVersion;
uint32_t bootId, generation = 0, connectionEpoch = 0, lastFullRefresh = 0;
bool hadValid = false;
ServerSnapshot latest;
char lastDevice[24] = {};
char lastDeviceState[64] = {};
char versionReportedTo[24] = {};
bool resetLatched = false;
NetworkRecoveryPolicy recovery;
portMUX_TYPE diagnosticsMux = portMUX_INITIALIZER_UNLOCKED;
NetworkDiagnostics diagnostics;
std::atomic<uint16_t> disconnectReason{0};
bool otaOperation = false;
int diagnosticHttpKind=0; // 0 unknown,1 own snapshot,2 unused (was peer read),3 send,4 chip report,5 management
// STA link bookkeeping. The Wi-Fi event task marks the first drop of an
// established link so later reconnect-attempt events never overwrite the
// original reason; the worker owns the outage timer and attempt counting.
std::atomic<bool> linkUp{false};
std::atomic<int8_t> lastRssi{0};
std::atomic<uint32_t> lossCount{0}, lossAtMs{0};
std::atomic<uint16_t> lossReason{0};
std::atomic<int8_t> lossRssi{0};
bool wifiWasUp = false, wifiDown = true, forcedReconnect = false;
uint32_t wifiDownSinceMs = 0, wifiUpSinceMs = 0, connectFailures = 0;

int8_t clampRssi(int value) {
  return static_cast<int8_t>(value < -127 ? -127 : (value > 0 ? 0 : value));
}

void noteLinkLost(uint16_t reason) {
  lossReason.store(reason);
  lossRssi.store(lastRssi.load());
  lossAtMs.store(millis());
  lossCount.fetch_add(1);
  has1blackbox::blackbox.log(has1blackbox::EventType::WifiEvent,reason,lastRssi.load(),0,0,"link.lost");
}

void beginOperation(NetworkPhase phase, uint32_t deadlineMs, uint32_t idleDeadlineMs = 0) {
  blackBoxOperation(int(phase));
  const uint32_t now = millis();
  portENTER_CRITICAL(&diagnosticsMux);
  diagnostics.phase = phase; ++diagnostics.sequence;
  diagnostics.startedAtMs = diagnostics.progressAtMs = now;
  diagnostics.deadlineMs = deadlineMs; diagnostics.idleDeadlineMs = idleDeadlineMs;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void finishOperation() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&diagnosticsMux);
  diagnostics.completedAtMs = now;
  portEXIT_CRITICAL(&diagnosticsMux);
  beginOperation(NetworkPhase::Idle, 20000);
}

void publishDiagnostics() {
  static uint32_t lastSample = 0;
  if (uint32_t(millis() - lastSample) < 1000) return;
  lastSample = millis();
  const auto result = wifi.LastHttpResult();
  // Sample potentially locking/allocating APIs outside our short copy lock.
  const uint32_t stack = uxTaskGetStackHighWaterMark(nullptr);
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const bool connected = linkUp.load();
  if (connected) lastRssi.store(clampRssi(WiFi.RSSI()));
  portENTER_CRITICAL(&diagnosticsMux);
  diagnostics.wifiConnected = connected;
  diagnostics.wifiWasUp = wifiWasUp;
  diagnostics.wifiDownSinceMs = wifiDownSinceMs;
  diagnostics.wifiConnectFailures = connectFailures;
  diagnostics.wifiLossCount = lossCount.load();
  diagnostics.wifiLastLossAtMs = lossAtMs.load();
  diagnostics.wifiLastLossReason = lossReason.load();
  diagnostics.wifiLastLossRssi = lossRssi.load();
  diagnostics.rssi = lastRssi.load();
  diagnostics.lastHttpError = static_cast<uint8_t>(result.error);
  diagnostics.lastHttpPhase = static_cast<uint8_t>(result.phase);
  diagnostics.lastHttpStatus = result.status;
  diagnostics.lastHttpElapsedMs = result.elapsedMs;
  diagnostics.lastHttpBytes = result.bytes;
  diagnostics.primaryTransportFailures = recovery.failures();
  diagnostics.forcedReconnects = recovery.reconnects();
  diagnostics.reconnectCooldownMs = recovery.cooldownMs();
  diagnostics.wifiDisconnectReason = disconnectReason.load();
  diagnostics.workerStackHighWater = stack;
  diagnostics.heapFree = freeHeap; diagnostics.heapLargest = largest;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void observeResponseBackoff(const HAS2HttpResult& result);

void httpProgress(HAS2HttpPhase phase, uint32_t bytes, void*) {
  static has1blackbox::EvidenceGate evidence[6];
  static uint32_t operation=0,lastSuccess[6]={};
  static bool inProgress=false;
  if (otaOperation) return;  // Keep completion acknowledgements inside the OTA exemption.
  if (phase == HAS2HttpPhase::Connect) {
    if(!inProgress) {++operation;inProgress=true;}
    beginOperation(NetworkPhase::Http, 20000);
    portENTER_CRITICAL(&diagnosticsMux);
    diagnostics.lastHttpError = 0; diagnostics.lastHttpStatus = 0;
    diagnostics.lastHttpElapsedMs = 0;
    portEXIT_CRITICAL(&diagnosticsMux);
  }
  else if (phase == HAS2HttpPhase::Complete) {
    const auto result = wifi.LastHttpResult();
    observeResponseBackoff(result);
    if(!inProgress)++operation;
    inProgress=false;
    const unsigned kind=unsigned(diagnosticHttpKind);
    const bool failed=result.error!=HAS2HttpError::None || result.status<200 || result.status>=300;
    if(evidence[kind].observe(millis(),failed,int(result.error),int(result.phase),result.status)) {
      has1blackbox::blackbox.log(has1blackbox::EventType::HttpSummary,operation,
          int(result.error),result.status,result.elapsedMs,failed?"http.failure":"http.recovered");
      has1blackbox::blackbox.log(has1blackbox::EventType::HttpSummary,operation,
          int(result.phase),result.bytes,has1blackbox::Unknown,"http.phase.bytes.errno");
      has1blackbox::blackbox.log(has1blackbox::EventType::HttpSummary,operation,
          lastSuccess[kind]?int32_t(millis()-lastSuccess[kind]):has1blackbox::Unknown,
          result.retryAfterMs,kind,"http.ok_age.retry.kind");
    }
    if(!failed) lastSuccess[kind]=millis();
    portENTER_CRITICAL(&diagnosticsMux);
    diagnostics.lastHttpError = static_cast<uint8_t>(result.error);
    diagnostics.lastHttpPhase = static_cast<uint8_t>(result.phase);
    diagnostics.lastHttpStatus = result.status;
    diagnostics.lastHttpElapsedMs = result.elapsedMs;
    diagnostics.lastHttpBytes = result.bytes;
    portEXIT_CRITICAL(&diagnosticsMux);
    finishOperation();
    return;
  }
  else {
    portENTER_CRITICAL(&diagnosticsMux);
    diagnostics.progressAtMs = millis();
    portEXIT_CRITICAL(&diagnosticsMux);
  }
  portENTER_CRITICAL(&diagnosticsMux);
  diagnostics.lastHttpPhase = static_cast<uint8_t>(phase);
  diagnostics.lastHttpBytes = bytes;
  portEXIT_CRITICAL(&diagnosticsMux);
}

void waitWorker(uint32_t durationMs) {
  beginOperation(NetworkPhase::Backoff, durationMs + 20000);
  publishDiagnostics();
  vTaskDelay(pdMS_TO_TICKS(durationMs));
  finishOperation();
}

void observeResponseBackoff(const HAS2HttpResult& result) {
  if (result.status == 429 || result.status == 503)
    recovery.defer(millis(), result.retryAfterMs ? result.retryAfterMs : kRetryMs);
}

bool serverRequestsDeferred() { return recovery.deferred(millis()); }

bool copyString(JsonVariantConst value, char* out, size_t size) {
  const char* text = value.as<const char*>();
  if (!text || strlen(text) >= size) return false;
  strcpy(out, text);
  return true;
}

bool number(JsonVariantConst value, long min, long max, long& out) {
  if (value.is<long>()) out = value.as<long>();
  else if (value.is<const char*>()) {
    const char* s = value.as<const char*>();
    if (!s || !*s) return false;
    char* end;
    errno = 0;
    out = strtol(s, &end, 10);
    if (errno || *end) return false;
  } else return false;
  return out >= min && out <= max;
}

bool liveName(const char* name) { return ChipReportPolicy::liveDevice(name); }

// ReceiveMine identifies the registration by this board's STA MAC. Its response
// need not contain a known game state; the absolute-write API rechecks MAC/key.
bool readChipIdentity() {
  diagnosticHttpKind=1;
  if (serverRequestsDeferred()) return false;
  char name[24];
  if (!wifi.ReceiveMineChecked() ||
      !copyString(my["device_name"], name, sizeof(name)) || !liveName(name)) {
    chipReport.disconnect();
    return false;
  }
  long ready, life;
  chipReport.bind(name, number(my["chip_report_ready"], 0, 1, ready) && ready == 1,
      number(my["life_chip"], 0, 1, life) ? static_cast<int>(life) : -1);
  return true;
}

bool decodeSnapshot(ServerSnapshot& out) {
  char game[24], role[24];
  if (!copyString(my["device_name"], out.deviceName, sizeof(out.deviceName)) || !liveName(out.deviceName) ||
      !copyString(my["game_state"], game, sizeof(game)) ||
      !copyString(my["device_state"], lastDeviceState, sizeof(lastDeviceState)) ||
      !copyString(my["role"], role, sizeof(role))) return false;
  if (!decodeServerStates(game, lastDeviceState, out)) return false;
  if (!strcmp(role, "player")) out.role = Role::Player;
  else if (!strcmp(role, "tagger")) out.role = Role::Tagger;
  else if (!strcmp(role, "ghost") || !strcmp(role, "revival")) out.role = Role::Ghost;
  else if (!strcmp(role, "neutral")) out.role = Role::Neutral;
  else return false;
  long count, seconds, sacrificed, open, life, vibe, brightness;
  if (!number(my["revival_count"], 0, 4, count) || !number(my["revival_time"], 1, 86400, seconds) ||
      !number(my["is_sacrificed"], 0, 1, sacrificed) || !number(my["is_open"], 0, 1, open) ||
      !number(my["life_chip"], 0, 100, life) ||
      // 0/1/3 proximity levels plus operator commands 10..17; anything else still rejects the snapshot.
      !number(my["vibe"], 0, feedback_config::kVibeCommandLast, vibe)) return false;
  out.revivalCount = count; out.stepSeconds = seconds;
  out.sacrificed = sacrificed; out.open = open; out.lifeChip = life;
  out.vibe = vibe;
  // Brightness converts here, as on the other HAS1 devices: server 1..100 maps onto raw
  // 1..255, and anything outside that range falls back to kDefaultBrightness instead of
  // rejecting the whole snapshot.
  out.brightness = number(my["brightness"], 1, 100, brightness)
      ? static_cast<uint8_t>(map(brightness, 1, 100, 1, 255))
      : kDefaultBrightness;
  ota::Command otaCommand;
  const auto parsedOta = ota::parseCommand(lastDeviceState, otaCommand);
  out.updateRequested = parsedOta == ota::ParseResult::Valid;
  if (parsedOta != ota::ParseResult::NotCommand) {
    // Malformed OTA strings stay identifiable but must never become latest.
    strncpy(out.updateCommand, lastDeviceState, sizeof(out.updateCommand) - 1);
  }
  if (out.updateRequested) {
    out.otaTtgoVersion = otaCommand.ttgo.version;
    out.otaBeetleVersion = otaCommand.beetle.version;
  }
  // The API has no game epoch. Use a local generation, invalidated on any loss
  // of synchronization or phase/device change. Never replay across a reconnect.
  const bool connectionChanged = !hadValid || strcmp(lastDevice, out.deviceName);
  if (connectionChanged) ++connectionEpoch;
  out.connectionEpoch = connectionEpoch;
  if (connectionChanged || out.role != latest.role || out.phase != latest.phase ||
      gameMutationsAllowed(out) != gameMutationsAllowed(latest)) ++generation;
  snprintf(out.session, sizeof(out.session), "%08lx-%lu", (unsigned long)bootId, (unsigned long)generation);
  strcpy(lastDevice, out.deviceName);
  out.receivedAtMs = millis(); out.valid = true;
  return true;
}

void publishInvalid() {
  hadValid = false;
  latest.valid = false;
  latest.updateRequested = latest.resetRequested = false;
  latest.vibe = 0;
  xQueueOverwrite(snapshots, &latest);
}

bool refresh() {
  if (serverRequestsDeferred()) { publishInvalid(); return false; }
  ServerSnapshot fresh;
  const bool identified = readChipIdentity();
  observeResponseBackoff(wifi.LastHttpResult());
  const bool valid = identified && decodeSnapshot(fresh);
  if (!valid) { publishInvalid(); return false; }
  fresh.receivedAtMs = millis();
  latest = fresh; hadValid = true; lastFullRefresh = millis();
  xQueueOverwrite(snapshots, &latest);
  return true;
}

bool send(const char* field, const String& value) {
  diagnosticHttpKind=3;
  return !serverRequestsDeferred() && wifi.SendChecked(latest.deviceName, field, value);
}

void execute(const GameEvent& event) {
  GameResult result{event.sequence, GameResult::Status::Rejected};
  blackBoxButtonCommand(event,1);
  // Read immediately before mutation. Snapshot freshness alone is not a lock,
  // but detects stale queues/phase changes without assuming a server transaction.
  if (!refresh()) result.status = GameResult::Status::Unknown;
  else if (commandAllowed(event, latest)) {
    bool ack = false;
    if (event.kind == GameEvent::Kind::SetCount) {
      blackBoxButtonCommand(event,2);
      ack = send("revival_count", String(event.value));
      blackBoxButtonCommand(event,3,ack);
    }
    blackBoxButtonCommand(event,4);
    const bool readBack = refresh();
    result.status = readBack && commandApplied(event, latest) ? GameResult::Status::Success :
        (ack && readBack ? GameResult::Status::Rejected : GameResult::Status::Unknown);
    if (result.status != GameResult::Status::Success)
      remoteConsoleLogf("[glove] command %lu unresolved; no automatic replay\n", (unsigned long)event.sequence);
  }
  // The consumer drains results before snapshots. One command in flight keeps
  // a known order for the remaining revival-count command.
  blackBoxButtonCommand(event,5,int(result.status));
  xQueueOverwrite(results, &result);
}

void observePhysicalChip() {
  const uint8_t present = physicalChip.load();
  if (present <= 1) chipReport.observe(present != 0);
}

void reportChip() {
  observePhysicalChip();
  if (!chipReport.due(millis())) return;
  // Bind immediately before writing, independently of full snapshot decoding.
  if (!readChipIdentity()) { publishInvalid(); return; }
  observePhysicalChip();
  ChipReportPolicy::Request request;
  // ReceiveMine cleared any shift_machine flag; read the snapshot on the next cycle.
  if (!chipReport.begin(millis(), request)) { forceSnapshot.store(true); return; }
  diagnosticHttpKind=4;
  if (!serverRequestsDeferred()) wifi.SetGloveChipChecked(request.device, request.present);
  const bool readBack = readChipIdentity();
  long value, ready;
  const char* device = my["device_name"] | "";
  const bool verified = readBack && !strcmp(device, request.device) &&
      number(my["life_chip"], 0, 1, value) && value == (request.present ? 1 : 0) &&
      number(my["chip_report_ready"], 0, 1, ready) && ready == 1;
  // A sensor update may arrive during either HTTP request. The old readback
  // acknowledges only its own value; a newer physical value remains pending.
  observePhysicalChip();
  chipReport.finish(request, verified, millis());
  if (!readBack) publishInvalid();
  forceSnapshot.store(true);  // Read any server-owned role change on the next loop.
  remoteConsoleLogf("[chip] absolute=%u readback=%s\n", request.present ? 1 : 0,
      verified ? "confirmed" : "pending");
}

bool checkedManagement() { diagnosticHttpKind=5;return wifi.LoopChecked(); }

// Loop is the periodic primary request: the server's liveness heartbeat and the
// carrier of shift_machine, watchdog and brightness. ReceiveMine follows only when
// due. `watchdog` is captured before later requests reuse the Loop document.
bool pollServer(bool forced, long& watchdog) {
  watchdog = -1;
  if (serverRequestsDeferred()) { publishInvalid(); return false; }
  bool valid = checkedManagement();
  auto primaryResult = wifi.LastHttpResult();
  observeResponseBackoff(primaryResult);
  if (!valid) publishInvalid();
  else {
    long flag, brightness;
    // Only an explicit 0 means unchanged; a missing or malformed flag re-reads.
    const bool flagged = !number(shift_machine["shift_machine"], 0, 0, flag);
    if (!number(shift_machine["watchdog"], 0, 1, watchdog)) watchdog = -1;
    // Same mapping as decodeSnapshot; a same-cycle ReceiveMine below still wins.
    if (number(shift_machine["brightness"], 1, 100, brightness))
      latest.brightness = static_cast<uint8_t>(map(brightness, 1, 100, 1, 255));
    if (fullRefreshDue(hadValid, forced, flagged, lastFullRefresh, millis())) {
      if (serverRequestsDeferred()) { publishInvalid(); return false; }
      // The cycle's outcome is then the snapshot read: a failed or semantically
      // invalid ReceiveMine counts as the primary failure, as it did before.
      valid = refresh();
      primaryResult = wifi.LastHttpResult();
    } else {
      // Unchanged: keep the last ReceiveMine content fresh for the main loop.
      latest.receivedAtMs = millis();
      xQueueOverwrite(snapshots, &latest);
    }
  }
  blackBoxHttp(int(primaryResult.error),primaryResult.status,primaryResult.elapsedMs,valid);
  recovery.observePrimary(millis(), primaryResult.error == HAS2HttpError::Transport, valid);
  if (!valid) {
    static uint32_t lastFailureLog = 0;
    if (uint32_t(millis() - lastFailureLog) >= 15000) {
      lastFailureLog = millis();
      remoteConsoleLogf("[net] poll error=%u http=%d elapsed=%lu bytes=%lu failures=%lu rssi=%d\n",
          static_cast<unsigned>(primaryResult.error), primaryResult.status,
          (unsigned long)primaryResult.elapsedMs, (unsigned long)primaryResult.bytes,
          (unsigned long)recovery.failures(), WiFi.RSSI());
    }
  }
  return valid;
}

void checkManagement(long watchdog) {
  if (serverRequestsDeferred() || !hadValid || watchdog < 0) return;
  if (!watchdog) { resetLatched = false; return; }
  if (!resetLatched && latest.phase != Phase::Active && otaState.load() != NetworkOtaStatus::Running &&
      send("watchdog", "0") && !serverRequestsDeferred() && checkedManagement() &&
      number(shift_machine["watchdog"], 0, 1, watchdog) && watchdog == 0) {
    latest.resetRequested = true;
    resetLatched = true;
    xQueueOverwrite(snapshots, &latest);
  }
}

void completeOtaCommand(const OtaRequest& request) {
  // Another version may have been requested while this update was running.
  // Only acknowledge the exact original server command; USB requests own none.
  if (request.sourceCommand[0] && refresh() &&
      latest.phase != Phase::Active && latest.phase != Phase::Photo &&
      !strcmp(lastDeviceState, request.sourceCommand)) send("device_state", "setting");
}

void performOta(const OtaRequest& request) {
#ifdef IOTGLOVE_COMPILE_ONLY
  otaState.store(NetworkOtaStatus::Failed);
  return;
#endif
  if (!refresh() || latest.phase == Phase::Active || latest.phase == Phase::Photo ||
      !HMAC_SECRET[0] || !strncmp(HMAC_SECRET, "REPLACE_WITH_", 13) ||
      strstr(HMAC_SECRET, "PLACEHOLDER") || strstr(HMAC_SECRET, "COMPILE_ONLY")) {
    remoteConsoleLogf("[OTA] TTGO preflight failed (server/phase/key configuration)\n");
    otaState.store(NetworkOtaStatus::Failed); return;
  }
  otaState.store(NetworkOtaStatus::Running);
  remoteConsoleLogf("[OTA] TTGO checking current=%d target=%lu (0=latest) partition=%d\n",
      version, (unsigned long)request.targetVersion, partitionVersion);
  // Preserve the deployed latest/pinned OTA protocols. Their blocking transfer
  // code has no progress contract for the new network-worker watchdog.
  // Scope cleanup restores normal monitoring on every returning path.
  struct OtaOperationScope {
    OtaOperationScope() {
      has1blackbox::blackbox.setPaused(true);
      otaOperation = true;
      beginOperation(NetworkPhase::Ota, 0);
    }
    ~OtaOperationScope() {
      has1blackbox::blackbox.setPaused(false);
      otaOperation = false;
      finishOperation();
    }
  } operation;
  if (request.targetVersion) {
    const auto result = ota::checkPinned("iotglove", request.targetVersion, version,
        partitionVersion, HMAC_SECRET, [request] {
          remoteConsoleLogf("[OTA] TTGO image verified; rebooting\n");
          completeOtaCommand(request);
        });
    if (result == ota::CheckResult::Skipped) {
      completeOtaCommand(request);
      otaState.store(NetworkOtaStatus::Skipped);
    } else otaState.store(NetworkOtaStatus::Failed);
    remoteConsoleLogf("[OTA] TTGO pinned result=%s\n", result == ota::CheckResult::Skipped ? "skipped" : "failed");
    return;
  }
  const String firmware = String(kRelease) + "update.bin";
  const String versionUrl = String(kRelease) + "version.txt";
  const String signature = String(kRelease) + "update.sig";
  SecureOTA ota(firmware.c_str(), versionUrl.c_str(), signature.c_str(), HMAC_SECRET, version);
  // Initial installation uses min_spiffs via USB. No in-place partition changes.
  ota.setOnSkip([request] {
    remoteConsoleLogf("[OTA] TTGO latest skipped\n");
    completeOtaCommand(request); otaState.store(NetworkOtaStatus::Skipped);
  });
  ota.setOnSuccess([request] {
    remoteConsoleLogf("[OTA] TTGO latest image verified; rebooting\n");
    completeOtaCommand(request);
  });
  ota.check();
  if (otaState.load() == NetworkOtaStatus::Running) {
    remoteConsoleLogf("[OTA] TTGO latest check failed\n");
    otaState.store(NetworkOtaStatus::Failed);
  }
}

// Queue invalidation publishes an explicit result so the main loop clears its
// in-flight token. Never synthesize a retry of an uncertain game mutation.
void cancelQueuedCommand() {
  GameEvent event;
  if (xQueueReceive(commands, &event, 0) == pdTRUE) {
    const GameResult result{event.sequence, GameResult::Status::Unknown};
    blackBoxButtonCommand(event,6,int(result.status));
    xQueueOverwrite(results, &result);
  }
}

void invalidateConnection() {
  chipReport.disconnect();
  publishInvalid();
  cancelQueuedCommand();
  if (otaState.load() == NetworkOtaStatus::Queued) {
    OtaRequest cancelled;
    xQueueReceive(otaRequests, &cancelled, 0);
    remoteConsoleLogf("[OTA] TTGO queued request cancelled: connection lost\n");
    otaState.store(NetworkOtaStatus::Failed);
  }
}

void worker(void*) {
  uint32_t lastPoll = 0;
  Location location{};
  Battery battery{};
  while (true) {
    if (WiFi.status() != WL_CONNECTED || WiFi.SSID() != "badland_shoot") {
      // A drop the event task did not see (SSID mismatch, missed event) still counts once.
      if (linkUp.exchange(false)) noteLinkLost(disconnectReason.load());
      if (!wifiDown) {
        wifiDown = true; wifiDownSinceMs = millis(); connectFailures = 0;
        if (forcedReconnect) remoteConsoleLogf("[net] link released for forced reconnect\n");
        else remoteConsoleLogf("[net] link lost reason=%u rssi=%d up_ms=%lu losses=%lu\n",
            lossReason.load(), lossRssi.load(), (unsigned long)(wifiDownSinceMs - wifiUpSinceMs),
            (unsigned long)lossCount.load());
      }
      forcedReconnect = false;
      invalidateConnection();
      recovery.disconnected();
      beginOperation(NetworkPhase::Connecting, 30000);
      has1blackbox::blackbox.log(has1blackbox::EventType::WifiTry,connectFailures,0,0,0,"connect.begin");
      const bool connected = wifi.TrySetupFixed("badland", "badland_shoot");
      finishOperation();
      has1blackbox::blackbox.log(has1blackbox::EventType::WifiTry,connected,connectFailures,0,0,"connect.end");
      if (!connected) { ++connectFailures; waitWorker(wifiConnectRetryMs(connectFailures)); continue; }
      const uint32_t now = millis();
      lastRssi.store(clampRssi(WiFi.RSSI()));
      linkUp.store(true);
      if (wifiWasUp || connectFailures)
        remoteConsoleLogf("[net] link restored after %lu ms failures=%lu rssi=%d last_loss reason=%u rssi=%d\n",
            (unsigned long)(now - wifiDownSinceMs), (unsigned long)connectFailures, lastRssi.load(),
            lossReason.load(), lossRssi.load());
      wifiWasUp = true; wifiDown = false; wifiUpSinceMs = now; connectFailures = 0;
      versionReportedTo[0] = 0;
      forceSnapshot.store(true);
    }
    if (recovery.deferred(millis())) {
      // HTTP Retry-After applies to all server traffic, including side lanes.
      // Keep monitoring Wi-Fi and queued command invalidation every 100ms.
      cancelQueuedCommand();
      waitWorker(100);
      continue;
    }
    // Let the button's Found pattern finish before any new request starts.
    if (pendingUntil(quietUntil.load(), millis(), kNetworkQuietMaxMs)) { waitWorker(10); continue; }
    if (otaState.load() == NetworkOtaStatus::Queued) {
      OtaRequest request;
      if (xQueueReceive(otaRequests, &request, 0) == pdTRUE) performOta(request);
      else otaState.store(NetworkOtaStatus::Failed);
    }
    GameEvent event;
    if (xQueueReceive(commands, &event, 0) == pdTRUE) execute(event);
    const bool forced = forceSnapshot.exchange(false);
    if (forced || uint32_t(millis() - lastPoll) >= kPollMs) {
      long watchdog;
      if (pollServer(forced, watchdog)) {
        if (strcmp(versionReportedTo, latest.deviceName) && send("esp_version", String(version)))
          strcpy(versionReportedTo, latest.deviceName);
        checkManagement(watchdog);
      }
      lastPoll = millis();
    }
    if (recovery.shouldReconnect(millis())) {
      invalidateConnection();
      has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,recovery.failures(),
          recovery.reconnects(),recovery.cooldownMs(),0,"http.force_reconnect");
      recovery.reconnected(millis(), esp_random());
      remoteConsoleLogf("[net] force reconnect=%lu cooldown=%lu reason=primary transport failure\n",
          (unsigned long)recovery.reconnects(), (unsigned long)recovery.cooldownMs());
      beginOperation(NetworkPhase::Connecting, 30000);
      // Do not erase credentials. TrySetupFixed alone returns immediately if
      // the STA still claims to be associated with this SSID.
      forcedReconnect = true;
      linkUp.store(false);  // Deliberate: the disconnect event is not a link loss.
      WiFi.disconnect(true, false);
      finishOperation();
      waitWorker(100);
      continue;
    }
    if (recovery.deferred(millis())) continue;
    reportChip();
    // The queue holds the main loop's latest report, which is "" (no location for
    // computeVibe) once the beacon is stale; its keepalive repeats a lost send.
    if (hadValid && xQueueReceive(locations, &location, 0) == pdTRUE) send("location", location.room);
    if (hadValid && xQueueReceive(batteries, &battery, 0) == pdTRUE &&
        uint32_t(millis() - battery.sampledAt) < 60000) {
      send("battery_remaining", String(battery.volts, 2));
    }
    waitWorker(10);
  }
}
}  // namespace

bool networkBegin(int firmwareVersion, int firmwarePartitionVersion) {
  if (snapshots) return true;
  version = firmwareVersion; partitionVersion = firmwarePartitionVersion; bootId = esp_random();
  wifi.SetDebugPrint(&remoteConsoleWifiPrint());
  wifi.EnableBoundedHttp();
  wifi.SetHttpProgressHook(httpProgress);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    has1blackbox::logWifiMetadata(event,info);
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
    const uint16_t reason = info.wifi_sta_disconnected.reason;
    disconnectReason.store(reason);
    // Only the first drop of an established link is a loss; the reconnect
    // attempts that follow raise their own disconnect events.
    if (linkUp.exchange(false)) noteLinkLost(reason);
  });
  wifiDownSinceMs = wifiUpSinceMs = millis();
  beginOperation(NetworkPhase::Starting, 30000);
  snapshots = xQueueCreate(1, sizeof(ServerSnapshot));
  commands = xQueueCreate(1, sizeof(GameEvent));
  results = xQueueCreate(1, sizeof(GameResult));
  locations = xQueueCreate(1, sizeof(Location));
  batteries = xQueueCreate(1, sizeof(Battery));
  otaRequests = xQueueCreate(1, sizeof(OtaRequest));
  if (!snapshots || !commands || !results || !locations || !batteries || !otaRequests) return false;
  return xTaskCreatePinnedToCore(worker, "glove_network", 12288, nullptr, 1, nullptr, 0) == pdPASS;
}

bool networkDiagnostics(NetworkDiagnostics& out) {
  if (!snapshots) return false;
  portENTER_CRITICAL(&diagnosticsMux);
  out = diagnostics;
  portEXIT_CRITICAL(&diagnosticsMux);
  return true;
}

bool networkPoll(ServerSnapshot& snapshot) { return snapshots && xQueueReceive(snapshots, &snapshot, 0) == pdTRUE; }
bool networkPollResult(GameResult& result) {
  if (!results || xQueueReceive(results, &result, 0) != pdTRUE) return false;
  commandBusy.store(false); return true;
}
bool networkSubmit(const GameEvent& event) {
  if (!commands || otaState.load() == NetworkOtaStatus::Queued || otaState.load() == NetworkOtaStatus::Running) return false;
  bool expected = false;
  if (!commandBusy.compare_exchange_strong(expected, true)) return false;
  if (xQueueSend(commands, &event, 0) == pdTRUE) return true;
  commandBusy.store(false); return false;
}
void networkRequestSnapshot() { forceSnapshot.store(true); }
void networkQuietUntil(uint32_t untilMs) { quietUntil.store(untilMs); }
void networkReportChip(bool present) { physicalChip.store(present ? 1 : 0); }
void networkReportLocation(const char* room) {
  if (!locations || !room || strlen(room) >= sizeof(Location::room)) return;
  Location sample{}; strcpy(sample.room, room);
  xQueueOverwrite(locations, &sample);
}
void networkReportBattery(float volts) {
  if (!batteries || !isfinite(volts) || volts <= 0 || volts > 60) return;
  Battery sample{volts, millis()}; xQueueOverwrite(batteries, &sample);
}
bool networkRequestOta(uint32_t targetVersion, const char* sourceCommand) {
  if (!otaRequests || commandBusy.load() || !sourceCommand ||
      strlen(sourceCommand) >= sizeof(OtaRequest::sourceCommand) ||
      targetVersion > INT32_MAX || otaState.load() != NetworkOtaStatus::Idle) return false;
  OtaRequest request;
  request.targetVersion = targetVersion;
  strcpy(request.sourceCommand, sourceCommand);
  // Called by the sole application loop. The worker sees Queued only after the
  // immutable request has been copied, and never consults later server targets.
  if (xQueueSend(otaRequests, &request, 0) != pdTRUE) return false;
  otaState.store(NetworkOtaStatus::Queued);
  return true;
}
NetworkOtaStatus networkOtaStatus() { return otaState.load(); }
void networkClearOtaStatus() {
  auto state = otaState.load();
  if (state == NetworkOtaStatus::Failed || state == NetworkOtaStatus::Skipped)
    otaState.compare_exchange_strong(state, NetworkOtaStatus::Idle);
}
}  // namespace iotglove
