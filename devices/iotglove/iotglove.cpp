#include "iotglove.h"

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_attr.h>
#include <WiFi.h>

#include "battery.h"
#include "sensor.h"
#include "feedback.h"
#include "motor_drive.h"
#include "pixel_refresh.h"
#include "blackbox.h"
#include "network_policy.h"
#include "network_restart.h"
#include "link_loss.h"
#include "state_policy.h"
#include "library_and_pin.h"
#include "wifi_client.h"
#include "peer_state.h"
#include "ota_request.h"
#include "link_diagnostics.h"
#include "telnet.h"
#include "telnet_policy.h"

namespace {
using namespace iotglove;

constexpr bool kTraining = IOTGLOVE_TRAINING != 0;
GameModel game(kTraining ? Profile::Training : Profile::Origin);
FeedbackEngine feedbackEngine;
motor_drive::SoftStart motorDrive;
PixelRefresh pixelRefresh;
BatterySampler battery(IOTGLOVE_BATTERY_DIVIDER_RATIO, IOTGLOVE_BATTERY_CALIBRATION,
                       IOTGLOVE_BATTERY_MIN_MV, IOTGLOVE_BATTERY_MAX_MV);
HardwareSerial beetleSerial(1);
Adafruit_NeoPixel pixels(IOTGLOVE_LED_COUNT, IOTGLOVE_LED_PIN, NEO_GRB + NEO_KHZ800);
wire::Decoder decoder;
PeerState peer;
OtaRequests otaRequests;
LinkDiagnostics linkDiagnostics;
DiagnosticLog diagnosticLog;
BeetleLogTracker beetleLogs;
int firmware = 0, partition = 0;
uint32_t bootId = 0, requestSequence = 0, lastPing = 0, lastHeart = 0;
uint32_t inFlight = 0, lastSnapshotRequest = 0, lastLocationReport = 0;
uint32_t lastBatterySample = 0, lastBatteryReport = 0;
uint32_t resetStarted = 0, lastReset = 0;
bool networkReady = false, desiredScan = false, modeKnown = false;
RTC_NOINIT_ATTR NetworkRestartHistory networkRestartHistory;
RTC_NOINIT_ATTR LinkLossRecord linkLossRecord;
NetworkRestartPolicy networkRestartPolicy;
esp_task_wdt_user_handle_t mainWatchdog = nullptr;
bool stallReported = false, linkDownReported = false;
uint32_t observedLossCount = 0;
// Context for the link-loss record: last valid server state, motor activity,
// and the raw battery ADC that is sampled even without a calibrated divider.
bool lastValidKnown = false;
Phase lastValidPhase = Phase::Unknown;
Role lastValidRole = Role::Neutral;
DeviceState lastValidDeviceState = DeviceState::Other;
bool motorEverOn = false;
uint32_t lastMotorOnMs = 0, lastBatteryAdcSample = 0;
uint16_t batteryAdcMv = 0;
float batteryVolts = 0;
bool resetLatched = false;
bool resetPending = false, resetHigh = false, resetTtgo = false, haveReset = false;
bool ttgoOtaSubmitted = false;
bool lastLocationValid = false, locationReported = false, haveOutputs = false;
char lastRoom[40] = {};
Outputs lastOutputs;
uint8_t lastBrightness = 0;
uint8_t lastMotorDuty = 0;
bool motorPwmReady = false;
uint32_t rxBytes = 0, rxLines = 0, rxDecoded = 0, rxRejectedLines = 0;
uint32_t txBytes = 0, txFrames = 0, txFailures = 0;
uint32_t statusPrintedAt = 0, manualProbeAt = 0, transitionPrintedAt = 0;
uint32_t resetReportAt = 0, reportedBoot = 0, probeReportId = 0;
bool statusPrinted = false, manualProbeSent = false, transitionPrinted = false;
bool reportedKnown = false, reportedOnline = false, probeReportPending = false;
ResetObservation reportedReset = ResetObservation::Never;
bool serverReported = false, reportedServerFresh = false, reportedServerValid = false;
Phase reportedPhase = Phase::Unknown;
char reportedDevice[sizeof(ServerSnapshot::deviceName)] = {};

uint32_t nextId() {
  if (++requestSequence == 0) ++requestSequence;
  return requestSequence;
}

bool send(const wire::Frame& frame) {
  char line[wire::kMaxLine + 1];
  const size_t length = wire::format(line, sizeof(line), frame);
  if (!length) { ++txFailures; return false; }
  const size_t written = beetleSerial.write(reinterpret_cast<const uint8_t*>(line), length);
  txBytes += static_cast<uint32_t>(written);
  if (written != length) { ++txFailures; return false; }
  ++txFrames;
  return true;
}

void sendMode() {
  wire::Frame frame;
  strcpy(frame.type, "MODE"); frame.id = nextId();
  wire::put(frame, desiredScan ? "1" : "0");
  wire::put(frame, kTraining ? "training" : "live");
  send(frame);
  modeKnown = true;
}

void hello(uint32_t id) {
  wire::Frame frame;
  strcpy(frame.type, "HELLO"); frame.id = id;
  wire::put(frame, "ttgo");
  char value[16];
  snprintf(value, sizeof(value), "%d", firmware); wire::put(frame, value);
  snprintf(value, sizeof(value), "%d", partition); wire::put(frame, value);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(bootId)); wire::put(frame, value);
  send(frame);
}

bool serverFresh(uint32_t now) {
  return serverSnapshotFresh(game.server(), now);
}

bool updatePhase() {
  return otaAllowedPhase(game.server().phase);
}

bool otaBusy() {
  const auto status = networkOtaStatus();
  return otaRequests.hasActive() || peer.busy(millis()) ||
      status == NetworkOtaStatus::Queued || status == NetworkOtaStatus::Running;
}

const char* probeName(ProbeState state) {
  switch (state) {
    case ProbeState::Waiting: return "waiting";
    case ProbeState::Matched: return "matched";
    case ProbeState::Timeout: return "timeout";
    case ProbeState::SendFailed: return "send_failed";
    default: return "never";
  }
}

const char* resetName(ResetObservation state) {
  switch (state) {
    case ResetObservation::Waiting: return "waiting";
    case ResetObservation::RebootObserved: return "reboot_observed";
    case ResetObservation::BaselineUnknown: return "baseline_unknown_not_confirmed";
    case ResetObservation::Timeout: return "timeout_not_confirmed";
    default: return "never";
  }
}

void ageText(char* out, size_t size, bool known, uint32_t age) {
  if (known) snprintf(out, size, "%lu", static_cast<unsigned long>(age));
  else snprintf(out, size, "unknown");
}

const char* phaseName(Phase phase) {
  switch (phase) {
    case Phase::Setting: return "setting";
    case Phase::Ready: return "ready";
    case Phase::Photo: return "photo";
    case Phase::Academy: return "academy";
    case Phase::Active: return "active";
    case Phase::Ended: return "ended";
    default: return "unknown";
  }
}

const char* roleName(Role role) {
  switch (role) {
    case Role::Neutral: return "neutral";
    case Role::Player: return "player";
    case Role::Tagger: return "tagger";
    case Role::Ghost: return "ghost";
    default: return "unknown";
  }
}

const char* motorAgeText(uint32_t now) {
  static char text[16];
  ageText(text, sizeof(text), motorEverOn, now - lastMotorOnMs);
  return text;
}

LinkLossContext currentLinkLossContext(uint32_t now) {
  LinkLossContext context;
  context.stateKnown = lastValidKnown;
  context.phase = static_cast<uint8_t>(lastValidPhase);
  context.role = static_cast<uint8_t>(lastValidRole);
  context.deviceState = static_cast<uint8_t>(lastValidDeviceState);
  context.motor = lastOutputs.motor;
  context.lit = lastOutputs.lit;
  context.motorAgeMs = motorEverOn ? uint32_t(now - lastMotorOnMs) : kLinkLossMotorNever;
  context.heapFree = ESP.getFreeHeap();
  context.batteryAdcMv = batteryAdcMv;
  context.batteryMv = batteryVolts > 0 ? static_cast<uint16_t>(batteryVolts * 1000.0f) : 0;
  return context;
}

// Printed once on the first boot after a recovery restart (or any reset that
// kept RTC RAM) so the Telnet collector sees why the link was lost even though
// the loss itself happened while nothing could be delivered.
void reportRetainedLinkLoss(esp_reset_reason_t resetReason) {
  if (!linkLossPresent(linkLossRecord)) return;
  const LinkLossRecord& r = linkLossRecord;
  has1blackbox::blackbox.log(has1blackbox::EventType::Crash,r.cause,r.reason,r.rssi,r.restartAttempt,"prior.link_loss");
  has1blackbox::blackbox.log(has1blackbox::EventType::Crash,r.lostAtMs,r.noticedAtMs,r.restartAtMs,r.connectFailures,"prior.loss_timing");
  has1blackbox::blackbox.log(has1blackbox::EventType::Crash,r.stateKnown?r.phase:-1,r.stateKnown?r.role:-1,r.motor,blackBoxRetainedAdcMv(r.batteryAdcMv),"prior.loss_context");
  char motorAge[16];
  ageText(motorAge, sizeof(motorAge), r.motorAgeMs != kLinkLossMotorNever, r.motorAgeMs);
  remoteConsoleLogf("[recovery] retained link loss: cause=%s reset_reason=%u reason=%u rssi=%d lost_uptime_ms=%lu noticed_uptime_ms=%lu restart_uptime_ms=%lu down_ms=%lu connect_failures=%lu attempt=%u\n",
      linkLossCauseName(static_cast<LinkLossCause>(r.cause)), static_cast<unsigned>(resetReason),
      r.reason, r.rssi, (unsigned long)r.lostAtMs, (unsigned long)r.noticedAtMs,
      (unsigned long)r.restartAtMs,
      (unsigned long)(r.restartAtMs && r.lostAtMs ? r.restartAtMs - r.lostAtMs : 0),
      (unsigned long)r.connectFailures, r.restartAttempt);
  remoteConsoleLogf("[recovery] retained context: phase=%s role=%s device_state=%s state_known=%u motor=%u motor_age_ms=%s lit=%u battery_adc_mv=%u battery_mv=%u heap=%lu\n",
      r.stateKnown ? phaseName(static_cast<Phase>(r.phase)) : "unknown",
      r.stateKnown ? roleName(static_cast<Role>(r.role)) : "unknown",
      r.stateKnown ? deviceStateName(static_cast<DeviceState>(r.deviceState)) : "unknown",
      r.stateKnown, r.motor, motorAge, r.lit, r.batteryAdcMv, r.batteryMv, (unsigned long)r.heapFree);
  linkLossClear(linkLossRecord);
}

void reportServerChanges(uint32_t now) {
  const ServerSnapshot& server = game.server();
  const bool fresh = serverFresh(now);
  if (serverReported && fresh == reportedServerFresh && server.valid == reportedServerValid &&
      server.phase == reportedPhase && !strcmp(server.deviceName, reportedDevice)) return;
  remoteConsoleLogf("[server] valid=%u fresh=%u device=%s phase=%s\n",
      server.valid, fresh, server.deviceName, phaseName(server.phase));
  serverReported = true; reportedServerFresh = fresh; reportedServerValid = server.valid;
  reportedPhase = server.phase;
  memcpy(reportedDevice, server.deviceName, sizeof(reportedDevice));
}

void printDiagnostics(uint32_t now) {
  if (statusPrinted && uint32_t(now - statusPrintedAt) < 1000U) return;
  statusPrinted = true; statusPrintedAt = now;
  char seen[16], helloAge[16], heartAge[16], rtt[16], resetAge[16];
  ageText(seen, sizeof(seen), peer.known(), peer.lastSeenAge(now));
  ageText(helloAge, sizeof(helloAge), peer.known(), peer.helloAge(now));
  ageText(heartAge, sizeof(heartAge), peer.heartbeatKnown(), peer.heartbeatAge(now));
  ageText(rtt, sizeof(rtt), linkDiagnostics.probeState() == ProbeState::Matched, linkDiagnostics.probeRtt());
  ageText(resetAge, sizeof(resetAge), linkDiagnostics.resetState() != ResetObservation::Never,
          linkDiagnostics.resetAge(now));
  // Fan-out directly: the complete status need not fit the small transition queue.
  const RemoteConsoleStatus console = remoteConsoleStatus();
  remoteConsoleLogf("[network] ip=%s MAC=%s boot=%lu telnet_listening=%u connected=%u\n"
      "[server] valid=%u fresh=%u device=%s phase=%s age_ms=%lu\n"
      "[beetle] reset_reason_known=%u reset_reason=%lu (explicit LOG snapshot)\n",
      WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), (unsigned long)bootId,
      console.listening, console.connected, game.server().valid, serverFresh(now),
      game.server().deviceName, phaseName(game.server().phase),
      (unsigned long)(now - game.server().receivedAtMs),
      beetleLogs.reasonKnown(peer.bootId()),
      (unsigned long)(beetleLogs.reasonKnown(peer.bootId()) ? beetleLogs.reason() : 0));
  remoteConsoleLogf("[remote] dropped_usb_bytes=%lu dropped_telnet_bytes=%lu rejected_commands=%lu\n",
      (unsigned long)console.usbDroppedBytes, (unsigned long)console.telnetDroppedBytes,
      (unsigned long)console.rejectedCommands);
  NetworkDiagnostics network;
  if (networkReady && networkDiagnostics(network)) {
    now = millis();  // Sample after the cross-core snapshot; its timestamps may be newer.
    remoteConsoleLogf("[recovery] wifi=%d rssi=%d disconnect_reason=%u phase=%u sequence=%lu age_ms=%lu progress_age_ms=%lu deadline_ms=%lu idle_ms=%lu\n",
        static_cast<int>(WiFi.status()), WiFi.RSSI(), network.wifiDisconnectReason,
        static_cast<unsigned>(network.phase), (unsigned long)network.sequence,
        (unsigned long)(now - network.startedAtMs), (unsigned long)(now - network.progressAtMs),
        (unsigned long)network.deadlineMs, (unsigned long)network.idleDeadlineMs);
    remoteConsoleLogf("[link] up=%u was_up=%u down_ms=%lu connect_failures=%lu losses=%lu last_loss_reason=%u last_loss_rssi=%d last_loss_age_ms=%lu restart_after_ms=%lu\n",
        network.wifiConnected, networkRestartPolicy.link().wasUp(),
        (unsigned long)networkRestartPolicy.link().downMs(now),
        (unsigned long)network.wifiConnectFailures, (unsigned long)network.wifiLossCount,
        network.wifiLastLossReason, network.wifiLastLossRssi,
        (unsigned long)(network.wifiLossCount ? now - network.wifiLastLossAtMs : 0),
        (unsigned long)networkRestartPolicy.link().limitMs());
    remoteConsoleLogf("[http] error=%u phase=%u status=%d elapsed_ms=%lu bytes=%lu primary_failures=%lu reconnects=%lu cooldown_ms=%lu\n",
        network.lastHttpError, network.lastHttpPhase, network.lastHttpStatus,
        (unsigned long)network.lastHttpElapsedMs, (unsigned long)network.lastHttpBytes,
        (unsigned long)network.primaryTransportFailures, (unsigned long)network.forcedReconnects,
        (unsigned long)network.reconnectCooldownMs);
    remoteConsoleLogf("[resources] heap=%lu largest=%lu worker_stack_min=%lu reset_reason=%u recovery_restarts=%lu main_wdt=%u\n",
        (unsigned long)network.heapFree, (unsigned long)network.heapLargest,
        (unsigned long)network.workerStackHighWater, static_cast<unsigned>(esp_reset_reason()),
        (unsigned long)networkRestartHistory.attempts, mainWatchdog != nullptr);
  }
  remoteConsoleLogf("[inputs] chip_gpio26=%d button_gpio27=%d chip_debounced=%u button_debounced=%u chip_model=%u\n",
      sensorChipRaw(), sensorButtonRaw(),
      sensorChipPresent(), sensorButtonDown(), game.chipPresent());
  const ServerSnapshot& server = game.server();
  remoteConsoleLogf("[game] role=%s synchronized=%u life_chip=%ld count=%u server_count=%u sacrificed=%u open=%u device_state=%s\n",
      roleName(server.role), game.synchronized(), (long)server.lifeChip,
      game.count(), server.revivalCount, server.sacrificed, server.open, deviceStateName(server.deviceState));
  // feedback() consumes event haptics. Diagnostics only read the last render.
  remoteConsoleLogf("[outputs] cache_valid=%u red=%u green=%u blue=%u lit=%u motor=%u motor_duty=%u brightness8=%u motor_age_ms=%s\n",
      haveOutputs, lastOutputs.red, lastOutputs.green, lastOutputs.blue, lastOutputs.lit,
      lastOutputs.motor, lastMotorDuty, lastBrightness, motorAgeText(now));
  remoteConsoleLogf("[battery] adc_mv=%u configured=%u volts=%.2f\n",
      batteryAdcMv, battery.configured(), static_cast<double>(batteryVolts));
  const has1blackbox::Diagnostics bb = has1blackbox::blackbox.diagnostics();
  remoteConsoleLogf("[blackbox] ready=%u status=%u partition=%lu flash_errors=%lu uploaded=%lu upload_failures=%lu dropped=%lu upload_stack=%lu\n",
      unsigned(bb.ready), unsigned(bb.ringStatus), (unsigned long)bb.partitionSize, (unsigned long)bb.flashErrors,
      (unsigned long)bb.uploaded, (unsigned long)bb.uploadFailures, (unsigned long)bb.dropped,
      (unsigned long)bb.uploaderStackFree);
  remoteConsoleLogf("[crash] state=%u task=%.15s pc=0x%08lx cause=%lu (coredump: 0=not checked 1=this reset 2=older 3=none)\n",
      unsigned(bb.crashState), bb.crashTask[0] ? bb.crashTask : "-",
      (unsigned long)bb.crashPc, (unsigned long)bb.crashCause);
  diagnosticLog.append(
      "[diag] TTGO fw=%d partition=%d; UART TX32->6 RX36<-5 reset12->1\n"
      "[peer] known=%u online=%u fw=%lu partition=%lu boot=%lu last_seen_ms=%s hello_age_ms=%s\n"
      "[heart] known=%u fresh=%u age_ms=%s uptime_ms=%lu scan=%u ota_busy=%u (last sample)\n"
      "[probe] latest=%s id=%lu age_ms=%lu rtt_ms=%s matched_fresh=%u sent=%lu matched=%lu timeouts=%lu\n"
      "[uart] rx_bytes=%lu lines=%lu decoded=%lu accepted=%lu rejected_lines=%lu tx_bytes=%lu frames=%lu failures=%lu\n"
      "[location] fresh=%u room=%s\n"
      "[reset] reset_pending=%u pulse_high=%u last_pulse_result=%s last_pulse_age_ms=%s baseline_known=%u baseline_boot=%lu observed_boot=%lu\n"
      "[console] s/?=status p=PING b=Beetle-reset u=OTA; dropped_logs=%lu\n",
      firmware, partition, peer.known(), peer.online(now), (unsigned long)peer.firmwareVersion(),
      (unsigned long)peer.partitionVersion(), (unsigned long)peer.bootId(), seen, helloAge,
      peer.heartbeatKnown(), peer.heartbeatFresh(now), heartAge, (unsigned long)peer.heartbeatUptime(),
      peer.heartbeatScanEnabled(), peer.heartbeatOtaBusy(), probeName(linkDiagnostics.probeState()),
      (unsigned long)linkDiagnostics.probeId(), (unsigned long)linkDiagnostics.probeAge(now), rtt,
      linkDiagnostics.matchedFresh(peer, now), (unsigned long)linkDiagnostics.probesSent(),
      (unsigned long)linkDiagnostics.probesMatched(), (unsigned long)linkDiagnostics.probeTimeouts(),
      (unsigned long)rxBytes, (unsigned long)rxLines, (unsigned long)rxDecoded,
      (unsigned long)peer.acceptedFrames(), (unsigned long)rxRejectedLines,
      (unsigned long)txBytes, (unsigned long)txFrames, (unsigned long)txFailures,
      peer.locationFresh(now), peer.locationFresh(now) ? peer.room(now) : "unknown",
      resetPending, resetHigh, resetName(linkDiagnostics.resetState()), resetAge,
      linkDiagnostics.resetBaselineKnown(),
      (unsigned long)linkDiagnostics.resetBaselineBoot(), (unsigned long)linkDiagnostics.resetObservedBoot(),
      (unsigned long)diagnosticLog.dropped());
}

void sendProbe(uint32_t now, bool manual) {
  if (manual) {
    if (manualProbeSent && uint32_t(now - manualProbeAt) < 250U) return;
    manualProbeSent = true; manualProbeAt = now;
  }
  if (linkDiagnostics.probeState() != ProbeState::Waiting) {
    const uint32_t id = nextId();
    if (!linkDiagnostics.beginProbe(id, now)) return;
    wire::Frame ping; strcpy(ping.type, "PING"); ping.id = id;
    if (!send(ping)) linkDiagnostics.probeSendFailed();
    lastPing = now;
  }
  if (manual) {
    probeReportPending = true; probeReportId = linkDiagnostics.probeId();
    diagnosticLog.append("[probe] request id=%lu state=%s\n", (unsigned long)probeReportId,
                         probeName(linkDiagnostics.probeState()));
  }
}

void reportDiagnosticChanges(uint32_t now) {
  const bool known = peer.known(), online = peer.online(now);
  if ((known != reportedKnown || online != reportedOnline || (known && peer.bootId() != reportedBoot)) &&
      (!transitionPrinted || uint32_t(now - transitionPrintedAt) >= 500U)) {
    const char* change = !reportedKnown ? "discovered" :
        (known && peer.bootId() != reportedBoot ? "new_boot" : (online ? "online" : "offline"));
    has1blackbox::blackbox.log(has1blackbox::EventType::Peer,peer.bootId(),
        peer.firmwareVersion(),peer.partitionVersion(),(known?1:0)|(online?2:0),"beetle.hello.state");
    transitionPrinted = true; transitionPrintedAt = now;
    if (diagnosticLog.append("[peer] %s known=%u online=%u fw=%lu partition=%lu boot=%lu\n", change,
        known, online, (unsigned long)peer.firmwareVersion(), (unsigned long)peer.partitionVersion(),
        (unsigned long)peer.bootId())) {
      reportedKnown = known; reportedOnline = online; reportedBoot = peer.bootId();
    }
  }
  if (probeReportPending && probeReportId == linkDiagnostics.probeId() &&
      linkDiagnostics.probeState() != ProbeState::Waiting) {
    diagnosticLog.append("[probe] id=%lu result=%s rtt_ms=%lu fresh=%u\n",
        (unsigned long)probeReportId, probeName(linkDiagnostics.probeState()),
        (unsigned long)(linkDiagnostics.probeState() == ProbeState::Matched ? linkDiagnostics.probeRtt() : 0),
        linkDiagnostics.matchedFresh(peer, now));
    probeReportPending = false;
  }
  const ResetObservation result = linkDiagnostics.resetState();
  if (result != ResetObservation::Never && result != ResetObservation::Waiting && result != reportedReset &&
      uint32_t(now - resetReportAt) >= 500U) {
    has1blackbox::blackbox.log(has1blackbox::EventType::Peer,int(result),
        linkDiagnostics.resetBaselineBoot(),linkDiagnostics.resetObservedBoot(),0,"beetle.reset.observed");
    resetReportAt = now;
    const char* evidence = result == ResetObservation::RebootObserved ? "new_boot+post_pulse_PING_HELLO" :
        (result == ResetObservation::BaselineUnknown ? "PING_HELLO_without_baseline" : "none");
    if (diagnosticLog.append("[reset] %s baseline_boot=%lu observed_boot=%lu evidence=%s; reset_reason_source=explicit_LOG_only\n",
        resetName(result), (unsigned long)linkDiagnostics.resetBaselineBoot(),
        (unsigned long)linkDiagnostics.resetObservedBoot(), evidence)) reportedReset = result;
  }
}

void flushDiagnostics() {
  if (!diagnosticLog.pending()) return;
  size_t count = diagnosticLog.pending();
  remoteConsoleWrite(reinterpret_cast<const uint8_t*>(diagnosticLog.data()), count);
  diagnosticLog.consumed(count);
}

void pollPeer(uint32_t now) {
  wire::Frame frame;
  // Bounded receive work: an incoming flood cannot starve sensors or feedback.
  for (unsigned n = 0; n < 128 && beetleSerial.available(); ++n) {
    const char byte = static_cast<char>(beetleSerial.read());
    ++rxBytes;
    const bool decoded = decoder.feed(byte, now, frame);
    if (byte == '\n') { ++rxLines; if (!decoded) ++rxRejectedLines; }
    if (!decoded) continue;
    ++rxDecoded;
    diagnostics::Log log;
    if (diagnostics::parseLog(frame, log) && beetleLogs.accept(log, peer.known(), peer.bootId())) {
      // Parser and boot/sequence tracker have validated this peer evidence.
      has1blackbox::blackbox.log(has1blackbox::EventType::Peer,log.bootId,log.uptimeMs,
          log.sequence,log.value,"beetle.log.values");
      has1blackbox::blackbox.log(has1blackbox::EventType::Peer,log.bootId,log.sequence,
          int(log.event),int(log.code),"beetle.log.kind");
      remoteConsoleLogf("[beetle] boot=%lu uptime_ms=%lu %s/%s value=%lu seq=%lu\n",
          (unsigned long)log.bootId, (unsigned long)log.uptimeMs, diagnostics::eventName(log.event),
          diagnostics::codeName(log.code), (unsigned long)log.value, (unsigned long)log.sequence);
    }
    if (peer.receive(frame, now)) {
      linkDiagnostics.observeHello(frame.id, peer, now);
      modeKnown = false; sendMode();
    }
    if (strcmp(frame.type, "PING") == 0 && frame.count == 0) hello(frame.id);
  }
  peer.tick(now);
  linkDiagnostics.tick(now);
  reportDiagnosticChanges(now);
  if (uint32_t(now - lastPing) >= 2000U) {
    sendProbe(now, false);
  }
  if (uint32_t(now - lastHeart) >= 1000U) {
    lastHeart = now;
    wire::Frame heart; strcpy(heart.type, "HEART"); heart.id = nextId();
    char value[16]; snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(now));
    wire::put(heart, value); wire::put(heart, desiredScan ? "1" : "0");
    wire::put(heart, otaBusy() ? "1" : "0"); send(heart);
  }
}

void pollNetwork(uint32_t now) {
  if (kTraining || !networkReady) return;
  GameResult result;
  while (networkPollResult(result)) {
    if (result.sequence != inFlight) continue;
    inFlight = 0;
    if (result.status != GameResult::Status::Success) {
      game.commandUncertain(result.sequence);
      networkRequestSnapshot();
      remoteConsoleLogf("[server] Command not confirmed; waiting for authoritative state\n");
    }
  }
  ServerSnapshot snapshot;
  while (networkPoll(snapshot)) {
    game.applyServer(snapshot, now);
    if (snapshot.valid) {
      lastValidKnown = true;
      lastValidPhase = snapshot.phase; lastValidRole = snapshot.role;
      lastValidDeviceState = snapshot.deviceState;
    }
    if (otaRequests.observe(snapshot)) remoteConsoleLogf("[OTA] Malformed version command rejected\n");
    if (snapshot.resetRequested && !resetLatched) { resetPending = true; resetTtgo = true; }
    resetLatched = snapshot.resetRequested;
  }
  if (!serverFresh(now) && game.synchronized()) game.commandUncertain(0);
  if ((!game.synchronized() || !serverFresh(now)) && uint32_t(now - lastSnapshotRequest) >= 2000U) {
    lastSnapshotRequest = now;
    networkRequestSnapshot();
  }
}

void submitGameEvent(uint32_t now) {
  if (kTraining || !networkReady || feedbackEngine.foundPending(now)) return;
  GameEvent event;
  if (game.peekEvent(event) && eventDue(event, now) && !inFlight &&
      !otaRequests.hasActive() && serverFresh(now) && networkSubmit(event)) {
    blackBoxButtonCommand(event,0);
    inFlight = event.sequence;
    game.consumeEvent();
  }
}

void controlOta(uint32_t now) {
  if (kTraining || !networkReady) return;
  // Latch complete command identity. Changing a target while one pair runs can
  // queue the next pair, but cannot alter either target in the active pair.
  if (otaRequests.hasPending() && !otaRequests.hasActive() && !inFlight && serverFresh(now) && updatePhase() &&
      !resetHigh && !resetPending && peer.ota() != PeerOta::Waiting && !ttgoOtaSubmitted) {
    const uint32_t id = nextId();
    if (peer.beginOta(id, now, otaRequests.pending().beetleVersion)) {
      otaRequests.startPending(now);
      wire::Frame frame; strcpy(frame.type, "OTA"); frame.id = id;
      if (otaRequests.active().beetleVersion) {
        char target[16];
        snprintf(target, sizeof(target), "%lu", static_cast<unsigned long>(otaRequests.active().beetleVersion));
        wire::put(frame, "version"); wire::put(frame, target);
      } else wire::put(frame, "check");
      send(frame);
      remoteConsoleLogf("[OTA] Checking Beetle before TTGO\n");
    }
  }
  if (otaRequests.abortReadyPair(now, peer.ota() == PeerOta::Ready,
      ttgoOtaSubmitted, serverFresh(now), game.server().phase)) {
    peer.clearOta();
    remoteConsoleLogf("[OTA] TTGO stage cancelled: phase changed or pair deadline elapsed\n");
  }
  if (otaRequests.hasActive() && peer.ota() == PeerOta::Ready && !ttgoOtaSubmitted && !inFlight &&
      serverFresh(now) && updatePhase()) {
    if (networkRequestOta(otaRequests.active().ttgoVersion, otaRequests.active().sourceCommand)) {
      ttgoOtaSubmitted = true;
      remoteConsoleLogf("[OTA] Beetle confirmed; checking TTGO\n");
    }
  }
  if (otaRequests.hasActive() && peer.ota() == PeerOta::Failed) {
    remoteConsoleLogf("[OTA] Beetle failed/timed out; TTGO update stopped\n");
    otaRequests.finishActive();
    peer.clearOta();
  }
  const auto status = networkOtaStatus();
  if (ttgoOtaSubmitted && (status == NetworkOtaStatus::Skipped || status == NetworkOtaStatus::Failed)) {
    remoteConsoleLogf("%s\n", status == NetworkOtaStatus::Skipped ? "[OTA] Both boards checked" : "[OTA] TTGO update failed");
    networkClearOtaStatus();
    ttgoOtaSubmitted = false;
    peer.clearOta();
    otaRequests.finishActive();
  }
}

// Restart paths spend a flash write before ESP.restart(); never leave the motor running across it.
void motorOff() {
  if (motorPwmReady) ledcWrite(IOTGLOVE_MOTOR_PIN, 0);
  lastMotorDuty = 0;
}

void controlReset(uint32_t now) {
  if (resetHigh && uint32_t(now - resetStarted) >= kResetPulseMs) {
    digitalWrite(IOTGLOVE_BEETLE_RESET_PIN, LOW);
    resetHigh = false;
    if (resetTtgo) { remoteConsoleLogf("[reset] Server watchdog: restarting TTGO\n"); motorOff(); blackBoxBeforeRestart(1,networkRestartHistory.attempts); ESP.restart(); }
  }
  if (resetPending && !resetHigh && !otaBusy() &&
      (!haveReset || uint32_t(now - lastReset) >= kResetCooldownMs)) {
    if (!resetTtgo) {
      linkDiagnostics.beginReset(peer, now);
      reportedReset = ResetObservation::Waiting;
      diagnosticLog.append("[reset] pulse HIGH %ums; fresh_HELLO_baseline=%u boot=%lu; observing up to %lums\n",
          kResetPulseMs, linkDiagnostics.resetBaselineKnown(),
          (unsigned long)linkDiagnostics.resetBaselineBoot(), (unsigned long)LinkDiagnostics::kResetTimeoutMs);
    }
    has1blackbox::blackbox.log(has1blackbox::EventType::Peer,resetTtgo,kResetPulseMs,
        peer.known(),peer.bootId(),"beetle.reset.pulse");
    digitalWrite(IOTGLOVE_BEETLE_RESET_PIN, HIGH);
    resetStarted = lastReset = now;
    resetHigh = haveReset = true;
    resetPending = false;
    peer.invalidateLocation();
    if (resetTtgo) remoteConsoleLogf("[reset] Beetle WDT request pulse\n");
  }
}

void reportLocation(uint32_t now) {
  if (kTraining || !networkReady) return;
  const bool valid = peer.locationFresh(now) && desiredScan;
  const char* room = valid ? peer.room(now) : "";
  // Immediately on change. The server recomputes vibe and tagger audio only on
  // a location Send, so an unchanged room is still repeated at a slow keepalive.
  if (!locationReported || valid != lastLocationValid || strcmp(room, lastRoom) != 0 ||
      uint32_t(now - lastLocationReport) >= 5000U) {
    networkReportLocation(room);  // Empty clears stale location, never "unknown".
    strncpy(lastRoom, room, sizeof(lastRoom) - 1);
    lastLocationValid = valid;
    locationReported = true;
    lastLocationReport = now;
  }
}

void sampleBattery(uint32_t now) {
  // The raw ADC is always sampled so the loss record and the status show the
  // pin even before the divider is calibrated (a floating pin reads ~0).
  if (uint32_t(now - lastBatteryAdcSample) >= 1000U) {
    lastBatteryAdcSample = now;
    const uint32_t mv = analogReadMilliVolts(IOTGLOVE_BATTERY_PIN);
    batteryAdcMv = mv > 0xFFFFU ? 0xFFFFU : static_cast<uint16_t>(mv);
    blackBoxAdcSample(now,mv);
  }
  if (!battery.configured() || uint32_t(now - lastBatterySample) < 20U) return;
  lastBatterySample = now;
  float voltage = 0;
  const uint32_t sample=analogReadMilliVolts(IOTGLOVE_BATTERY_PIN);
  blackBoxAdcSample(now,sample);
  if (!battery.add(sample, voltage)) return;
  batteryVolts = voltage;
  blackBoxBatteryVoltage(now);
  if (lastBatteryReport != 0 && uint32_t(now - lastBatteryReport) < 60000U) return;
  lastBatteryReport = now;
  remoteConsoleLogf("[battery] %.2f V\n", static_cast<double>(voltage));
  if (!kTraining && networkReady) networkReportBattery(voltage);
}

void render(uint32_t now) {
  const Feedback state = game.feedback();
  const bool academy = game.academy();
  const bool fresh = !kTraining && !academy && game.synchronized() && serverFresh(now) &&
      peer.locationFresh(now);
  // FeedbackEngine consumes Academy server vibe values without playing them,
  // preventing an unchanged command from replaying after Academy ends.
  Outputs out = feedbackEngine.update(state, game.server().vibe, fresh, now, otaBusy() || resetHigh);
  // Publish before motor ON, even when another button command is still queued.
  // Only actual playback renews this deadline; old events never overwrite it.
  if (!kTraining && networkReady && feedbackEngine.foundPending(now))
    networkQuietUntil(feedbackEngine.foundUntilMs());
  if (otaBusy() || resetHigh) out.motor = false;
  // out.motor stays the ON/OFF request for logs and diagnostics; the PWM duty
  // only shapes how an ON request starts. OFF always writes 0 immediately.
  lastMotorDuty = motorDrive.update(out.motor, out.motorSource, now);
  if (!motorPwmReady || !ledcWrite(IOTGLOVE_MOTOR_PIN, lastMotorDuty)) lastMotorDuty = 0;
  blackBoxMotor(now,out.motor,int(out.motorSource),out.motorPressId);
  blackBoxOutput(now,out.motor,lastOutputs.lit,lastBrightness);
  lastOutputs.motor = out.motor;  // Motor can change without any LED update.
  if (out.motor) { lastMotorOnMs = now; motorEverOn = true; }
  // Already raw 0..255 — the network task converted it when the snapshot was parsed.
  const uint8_t brightness = kTraining ? 255 : game.server().brightness;
  const bool visualChanged = !haveOutputs || out.red != lastOutputs.red || out.green != lastOutputs.green ||
      out.blue != lastOutputs.blue || out.lit != lastOutputs.lit || brightness != lastBrightness;
  // Rebuild from current output even on recovery resends, preserving intentional darkness.
  PixelRefresh nextRefresh = pixelRefresh;
  const bool refreshNeeded = nextRefresh.update(out.motor, visualChanged, now);
  if (!refreshNeeded) pixelRefresh = nextRefresh; // Track motor edges without consuming an output.
  if (refreshNeeded && has1blackbox::blackbox.tryBeginLedOutput()) {
    pixels.setBrightness(brightness);
    for (uint8_t n = 0; n < IOTGLOVE_LED_COUNT; ++n)
      pixels.setPixelColor(n, n < out.lit ? pixels.Color(out.red, out.green, out.blue) : 0);
    pixels.show();
    has1blackbox::blackbox.endLedOutput();
    pixelRefresh = nextRefresh; // Denied flash guard must not consume a resend.
    lastOutputs = out;
    lastBrightness = brightness;
    haveOutputs = true;
    blackBoxOutput(now,out.motor,out.lit,brightness);
  }
}
void handleConsoleCommand(char command, uint32_t now) {
    if (command == 'b') {
      if (!resetPending) {
        const char* waitFor = resetHigh ? "pulse_complete" : (otaBusy() ? "OTA" :
            (haveReset && uint32_t(now - lastReset) < kResetCooldownMs ? "cooldown" : "next_loop"));
        diagnosticLog.append("[reset] request accepted reset_pending=1 waiting_for=%s; last_pulse_result=%s belongs to the previous pulse\n",
            waitFor, resetName(linkDiagnostics.resetState()));
      }
      resetPending = true;
      if (!resetHigh) resetTtgo = false;
    }
    if (command == 'u' && !kTraining) otaRequests.requestLatest();
    if (command == 's' || command == '?') printDiagnostics(now);
    if (command == 'p') sendProbe(now, true);
}
void armMainWatchdog() {
  if (mainWatchdog) return;  // Retry history never disables actual main-loop hang detection.
  // Preserve the configured idle subscribers. The loop never blocks on the
  // network (the worker does, under its own 20s limit), so a pass takes
  // milliseconds; 15s leaves flash erases three times the core's 5s default.
  esp_task_wdt_config_t config{};
  config.timeout_ms = 15000;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  config.idle_core_mask |= 1U;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  config.idle_core_mask |= 2U;
#endif
  config.trigger_panic = true;
  esp_err_t err = esp_task_wdt_reconfigure(&config);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&config);
  if (err == ESP_OK) err = esp_task_wdt_add_user("glove-main-progress", &mainWatchdog);
  if (err != ESP_OK) remoteConsoleLogf("[recovery] main watchdog unavailable error=%d\n", err);
}

void monitorNetwork() {
  if (!networkReady) return;
  NetworkDiagnostics d;
  if (!networkDiagnostics(d)) return;
  const uint32_t now = millis();  // Never compare a newer worker sample to an older loop time.
  // Capture a new link loss once, as close to the event as the worker's 1s publish allows.
  if (d.wifiLossCount != observedLossCount) {
    observedLossCount = d.wifiLossCount;
    const LinkLossContext context = currentLinkLossContext(now);
    has1blackbox::blackbox.log(has1blackbox::EventType::State,
        context.stateKnown?context.phase:-1,context.stateKnown?context.role:-1,context.motor,
        context.batteryMv?context.batteryMv:-1,"loss.context");
    has1blackbox::blackbox.log(has1blackbox::EventType::State,
        context.motorAgeMs,context.lit,blackBoxAdcMv(context.batteryAdcMv,lastBatteryAdcSample!=0),context.heapFree,"loss.output_adc_heap");
    linkLossCapture(linkLossRecord, now, d.wifiLastLossAtMs, d.wifiLastLossReason, d.wifiLastLossRssi,
                    d.wifiConnectFailures, context);
    remoteConsoleLogf("[recovery] link loss captured reason=%u rssi=%d age_ms=%lu phase=%s role=%s device_state=%s motor=%u motor_age_ms=%s battery_adc_mv=%u\n",
        d.wifiLastLossReason, d.wifiLastLossRssi, (unsigned long)(now - d.wifiLastLossAtMs),
        context.stateKnown ? phaseName(lastValidPhase) : "unknown",
        context.stateKnown ? roleName(lastValidRole) : "unknown",
        context.stateKnown ? deviceStateName(lastValidDeviceState) : "unknown",
        context.motor, motorAgeText(now), context.batteryAdcMv);
  }
  if (d.wifiConnected && linkLossPending(linkLossRecord)) {
    remoteConsoleLogf("[recovery] link loss resolved without restart down_ms=%lu\n",
        (unsigned long)(now - linkLossRecord.lostAtMs));
    linkLossClear(linkLossRecord);
  }
  const bool ota = otaBusy() || d.phase == NetworkPhase::Ota;
  const bool stalled = !ota && networkWorkerStalled(d, now);
  // poll() advances the main loop's own outage clock and decides the restart;
  // the latches below limit diagnostics only.
  const NetworkRestartDecision decision = networkRestartPolicy.poll(
      networkRestartHistory,d,now,ota);
  const LinkOutageClock& link = networkRestartPolicy.link();
  const bool linkDown = !ota && link.tooLong(now);
  if (!stalled) stallReported = false;
  else if (!stallReported) {
    stallReported = true;
    has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,int(d.phase),d.sequence,
        now-d.startedAtMs,now-d.progressAtMs,"worker.stall");
    remoteConsoleLogf("[recovery] worker stalled phase=%u sequence=%lu age_ms=%lu idle_ms=%lu restarts=%lu\n",
        static_cast<unsigned>(d.phase), (unsigned long)d.sequence,
        (unsigned long)(now-d.startedAtMs), (unsigned long)(now-d.progressAtMs),
        (unsigned long)networkRestartHistory.attempts);
  }
  if (!linkDown) linkDownReported = false;
  else if (!linkDownReported) {
    linkDownReported = true;
    has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,link.downMs(now),
        d.wifiConnectFailures,link.wasUp(),d.wifiLastLossReason,"wifi.down.threshold");
    remoteConsoleLogf("[recovery] wifi down %lu ms was_up=%u connect_failures=%lu restarts=%lu\n",
        (unsigned long)link.downMs(now),link.wasUp(),
        (unsigned long)d.wifiConnectFailures,(unsigned long)networkRestartHistory.attempts);
  }
  if (decision.restored) {
    has1blackbox::blackbox.log(has1blackbox::EventType::Recovery,decision.previousAttempts,
        NetworkRestartPolicy::kStableMs,d.sequence,0,"restart.backoff_reset");
    remoteConsoleLogf("[recovery] link connected and worker progressing for 10 minutes; restart count reset\n");
  }
  if (decision.restart) {
    const auto cause = decision.cause == 2 ? LinkLossCause::WorkerStall : LinkLossCause::WifiDown;
    linkLossComplete(linkLossRecord,cause,now,d.wifiConnectFailures,
        NetworkRestartPolicy::diagnosticAttempt(networkRestartHistory.attempts),currentLinkLossContext(now));
    motorOff();
    blackBoxBeforeRestart(decision.cause,networkRestartHistory.attempts);
    ESP.restart();
  }
}
}  // namespace

void gloveBegin(int firmwareVersion, int partitionVersion) {
  firmware = firmwareVersion; partition = partitionVersion;
  // Set output latch before enabling the driver: motor OFF and reset idle LOW.
  digitalWrite(IOTGLOVE_MOTOR_PIN, LOW); pinMode(IOTGLOVE_MOTOR_PIN, OUTPUT);
  // Hardware PWM for the motor MOSFET: duty 0 keeps the pin LOW, so the gate
  // stays off across the attach. Above the audible band; 8-bit is plenty.
  motorPwmReady = ledcAttach(IOTGLOVE_MOTOR_PIN, IOTGLOVE_MOTOR_PWM_HZ, IOTGLOVE_MOTOR_PWM_BITS);
  if (motorPwmReady) ledcWrite(IOTGLOVE_MOTOR_PIN, 0);
  digitalWrite(IOTGLOVE_BEETLE_RESET_PIN, LOW); pinMode(IOTGLOVE_BEETLE_RESET_PIN, OUTPUT);
  sensorConfigurePins();
  Serial.begin(115200);
  beetleSerial.begin(IOTGLOVE_UART_BAUD, SERIAL_8N1, IOTGLOVE_UART_RX, IOTGLOVE_UART_TX);
  blackBoxBegin(firmware, partition);
  pixels.begin(); pixels.clear();
  if (has1blackbox::blackbox.tryBeginLedOutput()) {
    pixels.show();
    has1blackbox::blackbox.endLedOutput();
  } // First main render retries the current frame if writer owned the flash guard.
  const uint32_t now = millis();
  sensorBegin(now);
  game.begin(sensorChipPresent(), now);
  bootId = esp_random(); requestSequence = esp_random();
  if (!remoteConsoleBegin(firmware, partition, bootId, !kTraining))
    remoteConsoleLogf("[remote] initialization failed; USB diagnostics remain available\n");
  // Raw ADC sampling of the battery pin runs regardless of calibration.
  analogReadResolution(12);
  analogSetPinAttenuation(IOTGLOVE_BATTERY_PIN, ADC_11db);
  remoteConsoleLogf("[glove] firmware=%d profile=%s motor_pwm=%u\n", firmware, kTraining ? "training" : "origin", motorPwmReady);
  if (!motorPwmReady) remoteConsoleLogf("[motor] ledcAttach failed; GPIO13 stays LOW and the motor never runs\n");
  diagnosticLog.append("[diag] TTGO console ready: s/? status, p UART probe, b Beetle reset, u OTA\n");
  if (!battery.configured())
    remoteConsoleLogf("[battery] Calibrated reporting disabled: configure measured ADC divider and battery range; raw adc_mv is still sampled\n");
  if (!kTraining) {
    const esp_reset_reason_t reason = esp_reset_reason();
    NetworkRestartPolicy::boot(networkRestartHistory, reason == ESP_RST_SW,
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_WDT || reason == ESP_RST_PANIC);
    reportRetainedLinkLoss(reason);
    networkReady = networkBegin(firmware, partition);
    if (networkReady) {
      networkReportChip(sensorChipPresent());
      armMainWatchdog();
      remoteConsoleLogf("[recovery] reset_reason=%u attempts=%lu main_wdt=%u\n",
          static_cast<unsigned>(reason),(unsigned long)networkRestartHistory.attempts,mainWatchdog != nullptr);
    }
  }
  hello(nextId()); sendMode();
}

void gloveLoop() {
  const uint32_t now = millis();
  pollPeer(now);
  pollNetwork(now);
  if (!kTraining) reportServerChanges(now);
  sensorPoll(game, now, inFlight);
  if (!kTraining && networkReady) networkReportChip(sensorChipPresent());
  game.tick(now);
  // Read-only status and bounded PING probes work without a server connection.
  for (unsigned n = 0; n < 8 && Serial.available(); ++n) {
    const char command = static_cast<char>(Serial.read());
    handleConsoleCommand(command, now);
  }
  char remoteCommand;
  for (unsigned n = 0; n < 4 && remoteConsoleReadCommand(remoteCommand); ++n)
    handleConsoleCommand(remoteCommand, now);
  controlOta(now);
  controlReset(now);
  const bool scan = !kTraining && serverFresh(now) && game.server().phase == Phase::Active && !otaBusy();
  if (scan != desiredScan) { desiredScan = scan; modeKnown = false; if (!scan) peer.invalidateLocation(); }
  if (!modeKnown && peer.online(now)) sendMode();
  reportLocation(now);
  sampleBattery(now);
  render(now);
  // Admit commands after this loop's input and haptics, including motor OFF.
  submitGameEvent(now);
  flushDiagnostics();
  remoteConsolePoll();
  blackBoxLoop(millis(),int(game.server().phase),int(game.server().role),int(game.server().deviceState),
      game.synchronized() && serverFresh(millis()),otaBusy(),lastOutputs.motor,lastOutputs.lit,
      blackBoxAdcMv(batteryAdcMv,lastBatteryAdcSample!=0),
      battery.configured() && batteryVolts>0 ? int(batteryVolts*1000) : -1,peer.online(millis()));
  monitorNetwork();
  // Feed only after completing a full main-loop pass, including the worker check.
  if (mainWatchdog) esp_task_wdt_reset_user(mainWatchdog);
  delay(1);  // Yield to Wi-Fi/RTOS; all timers above are nonblocking.
}
