// Each connector uses the same 8-of-10 sliding vote in every active stage.
// Real 100 ms task samples; startup waits 1 s and gaps do not expire the verdict.
static const uint8_t WIRE_PINS[4] = { WIRE_PIN_1, WIRE_PIN_2, WIRE_PIN_3, WIRE_PIN_4 };
static GeneratorWireState wireState;
static portMUX_TYPE wireStateMux = portMUX_INITIALIZER_UNLOCKED;
static bool wireSamplingEnabled = false; // Access only under wireStateMux.
static StaticTask_t wireSamplerControl;
static StackType_t wireSamplerStack[2048];
static TaskHandle_t wireSamplerTask = nullptr;
static int wireLastPresented = -1; // Main-task presentation, not an event queue.
static bool wireEnabled = false;
static bool wireContextValid = false;
static bool wireRetryPending = false;
static bool wireFenceNeeded = true; // Fence requests left pending by a previous ESP boot.
// The server declared battery_max for a cycle this device never completed itself
// (dashboard override). Physical wires no longer gate that cycle; reset clears it.
static bool wireServerCompleted = false;
static uint32_t wireLastAttempt = 0;
static GeneratorWireSnapshot wireContext;
static char wireObservedGame[32] = {};
static char wireObservedDevice[32] = {};
static char wireObservedName[32] = {};

static bool WireTextEquals(const char *a, const char *b) { return a && b && strcmp(a,b) == 0; }
static bool WireMonitoredState(const char *state) {
    return WireTextEquals(state,"activate") || WireTextEquals(state,"battery_max") ||
           WireTextEquals(state,"starter_finish") || WireTextEquals(state,"repaired") ||
           WireTextEquals(state,"repaired_all") || WireTextEquals(state,"tagger");
}

static GeneratorWireState WireSnapshot() {
    portENTER_CRITICAL(&wireStateMux);
    const GeneratorWireState snapshot = wireState;
    portEXIT_CRITICAL(&wireStateMux);
    return snapshot;
}
static void WireInvalidateAck() {
    portENTER_CRITICAL(&wireStateMux);
    wireState.invalidateAck();
    portEXIT_CRITICAL(&wireStateMux);
}
static bool WireAcknowledge(int count, uint32_t generation) {
    portENTER_CRITICAL(&wireStateMux);
    const bool accepted = wireState.acknowledge(count, generation);
    portEXIT_CRITICAL(&wireStateMux);
    DiagnosticPoint(GeneratorDiagnostic::Wire,accepted?1:-1,generation);
    BlackBoxTrace(GeneratorTrace::WireAck,count,int32_t(generation),accepted);
    DiagnosticPrintf("[GD] t=%lu wire_ack accepted=%u count=%d generation=%lu\r\n",
        (unsigned long)millis(),accepted,count,(unsigned long)generation);
    return accepted;
}
static uint8_t WireReadMask() {
    uint8_t mask = 0;
    for (int i = 0; i < 4; ++i) if (digitalRead(WIRE_PINS[i]) == LOW) mask |= (1u << i);
    return mask;
}
// Called only by the input task. No JSON, logging, audio or network work here.
static void WireSamplerTick() {
    portENTER_CRITICAL(&wireStateMux);
    const bool enabled = wireSamplingEnabled;
    const uint32_t generation = wireState.generation();
    portEXIT_CRITICAL(&wireStateMux);
    if (!enabled) return;
    const uint8_t mask = WireReadMask();
    const uint32_t observedAt = millis();
    portENTER_CRITICAL(&wireStateMux);
    if (wireSamplingEnabled && wireState.generation() == generation) wireState.sampleMask(observedAt,mask);
    portEXIT_CRITICAL(&wireStateMux);
}
static void WireSamplerTaskMain(void *) {
    for (;;) {
        WireSamplerTick();
        // No catch-up samples: use the actual GPIO observation time each turn.
        vTaskDelay(pdMS_TO_TICKS(GeneratorWireState::SampleMs));
    }
}
void WireInit() {
    for (int i = 0; i < 4; ++i) pinMode(WIRE_PINS[i], INPUT_PULLUP);
    if (!wireSamplerTask) {
        wireSamplerTask = xTaskCreateStaticPinnedToCore(WireSamplerTaskMain,"wire-input",
            sizeof(wireSamplerStack),nullptr,2,wireSamplerStack,&wireSamplerControl,tskNO_AFFINITY);
        if (!wireSamplerTask) Serial.println("[Wire] input task creation failed; completion disabled");
    }
}
int WireCountPlugged() {
    int count = 0;
    for (int i = 0; i < 4; ++i) if (digitalRead(WIRE_PINS[i]) == LOW) ++count;
    return count;
}
int WireDisplayCount() { const int count = WireSnapshot().stable(); return count < 0 ? 0 : count; }

void WireResetTracking() {
    GeneratorNetworkInvalidateGame();
    portENTER_CRITICAL(&wireStateMux);
    wireState.reset();
    wireSamplingEnabled = wireEnabled;
    portEXIT_CRITICAL(&wireStateMux);
    wireLastPresented = -1;
    Mp3BatteryCancel();
    wireContextValid = false;
    wireFenceNeeded = true; // Re-entry/identity changes must fence any earlier in-flight write.
    wireServerCompleted = false;
    batteryFinishDone = false;
    batteryFinishAudioPlayed = false;
    // Never publish raw input or finish while (re)entering a charge cycle.
    Serial.println("[Wire] reset: waiting for 1 s startup, 8/10 input votes and server acknowledgement");
}

void WireObserveServerSnapshot() {
    const char *game = my["game_state"];
    const char *state = my["device_state"];
    const char *name = my["device_name"];
    const bool enabled = WireTextEquals(game,"activate") && WireMonitoredState(state);
    const bool enteringCharge = enabled && WireTextEquals(state,"activate") &&
        !WireTextEquals(wireObservedDevice,"activate");
    const bool newIdentity = name && !WireTextEquals(name,wireObservedName);
    const bool changedServerState = !WireTextEquals(game,wireObservedGame) ||
        !WireTextEquals(state,wireObservedDevice);
    if (newIdentity) wireContext = GeneratorWireSnapshot();
    const bool resetInputs = enabled != wireEnabled || enteringCharge || newIdentity;
    wireEnabled = enabled;
    if (resetInputs) WireResetTracking();
    // State writes also consume CAS revisions. Refresh that context without
    // replaying completion audio or resetting a tagger/starter session.
    if (changedServerState) wireContextValid = false;
    const GeneratorWireState observed = WireSnapshot();
    if (wireContextValid && observed.acknowledged() &&
        ((int)my["battery_pack"] != observed.acknowledgedCount() ||
         (int)my["max_battery_pack"] != wireContext.maximum)) {
        WireInvalidateAck(); wireContextValid = false;
    }
    snprintf(wireObservedGame,sizeof(wireObservedGame),"%s",game ? game : "");
    snprintf(wireObservedDevice,sizeof(wireObservedDevice),"%s",state ? state : "");
    snprintf(wireObservedName,sizeof(wireObservedName),"%s",name ? name : "");
}

// Main consumes only the latest state; it does not replay missed transitions.
void WireSampleInputs(bool) {
    if (!wireEnabled) return;
    const GeneratorWireState snapshot = WireSnapshot();
    if (!snapshot.qualified(millis()) || snapshot.stable() == wireLastPresented) return;
    // Persist the exact presentation input before LED/audio can block. Text
    // snapshots are separately rate-limited; this compact transition is not.
    DiagnosticPoint(GeneratorDiagnostic::Wire,snapshot.stable(),
        uint32_t(snapshot.rawMask()) | (uint32_t(snapshot.connectedVotes(0))<<4) |
        (uint32_t(snapshot.connectedVotes(1))<<8) | (uint32_t(snapshot.connectedVotes(2))<<12) |
        (uint32_t(snapshot.connectedVotes(3))<<16));
    const int previous = wireLastPresented;
    wireLastPresented = snapshot.stable();
    // RAM-captured copy of the same presentation; logged to flash later, away from audio.
    BlackBoxTrace(GeneratorTrace::WirePresent,snapshot.stable(),previous,
        int32_t(uint32_t(snapshot.rawMask()) | (uint32_t(snapshot.connectedVotes(0))<<4) |
        (uint32_t(snapshot.connectedVotes(1))<<8) | (uint32_t(snapshot.connectedVotes(2))<<12) |
        (uint32_t(snapshot.connectedVotes(3))<<16) | (ptrCurrentMode == WirePollMain ? (1u<<24) : 0u) |
        (batteryFinishAudioPlayed ? (1u<<25) : 0u)));
    Serial.printf("[Wire] stable=%d previous=%d maximum=%d\n", snapshot.stable(), previous, (int)my["max_battery_pack"]);
    if (ptrCurrentMode == WirePollMain) {
        BatteryPackSend();
        if (!batteryFinishAudioPlayed && previous >= 0 && snapshot.stable() > previous) Mp3PlayLargeFolder(1,7);
    }
}

static bool WireSnapshotCanProgress(const GeneratorWireState &snapshot) {
    // Server override still requires a live, monitored activate stage.
    if (wireServerCompleted) return wireEnabled && WireTextEquals(my["game_state"],"activate");
    return wireSamplerTask && wireEnabled && wireContextValid && WireTextEquals(my["game_state"],"activate") &&
        wireContext.maximum == (int)my["max_battery_pack"] &&
        snapshot.ready(millis(), (int)my["max_battery_pack"]);
}
bool WireCanProgress() { return WireSnapshotCanProgress(WireSnapshot()); }
// Accept a server-side battery_max this device did not reach physically, so the
// starter and repair can proceed. Physical counts are still reported to the server.
void WireAcceptServerCompletion() {
    if (wireServerCompleted) return;
    wireServerCompleted = true;
    Serial.println("[Wire] server declared battery_max; physical wire gate bypassed for this cycle");
}
bool WireServerCompleted() { return wireServerCompleted; }
bool WireReadyForCompletion() {
    // Connection, disconnection and completion share the same filtered verdict.
    // A single raw read must not override the 8-of-10 window or add a sample.
    return WireSnapshotCanProgress(WireSnapshot());
}

static bool WireAdoptContext(const GeneratorWireSnapshot &snapshot) {
    const bool changedEpoch = wireContext.epoch[0] && strcmp(wireContext.epoch,snapshot.epoch) != 0;
    const bool stateMismatch = !WireTextEquals(snapshot.gameState,my["game_state"]) ||
        !WireTextEquals(snapshot.deviceState,my["device_state"]) ||
        snapshot.maximum != (int)my["max_battery_pack"];
    if (!changedEpoch && wireContext.epoch[0] && snapshot.revision < wireContext.revision) {
        WireInvalidateAck(); wireContextValid = false;
        Serial.println("[WireSync] stale revision rejected");
        return false;
    }
    wireContext = snapshot;
    if (changedEpoch || stateMismatch) {
        const bool resetCycle = changedEpoch || !WireTextEquals(snapshot.gameState,my["game_state"]) ||
            (WireTextEquals(snapshot.deviceState,"activate") && !WireTextEquals(my["device_state"],"activate"));
        if (resetCycle) WireResetTracking();
        else { WireInvalidateAck(); wireContextValid = false; }
        // The CAS reply is a partial object, never replace the full device JSON.
        // A failed legacy refresh leaves the acknowledgement invalid; the next
        // bounded retry fetches and checks the CAS context again.
        if(changedEpoch)GeneratorNetworkRefreshCycle();
        else GeneratorNetworkRefresh();
        if (resetCycle) receiveMineOn = false;
        Serial.println("[WireSync] server context changed; full state refresh requested");
        return false;
    }
    wireContextValid = true;
    return true;
}

static void WireDiagnosticSnapshot(const GeneratorWireState &s) {
    static bool first=true;
    static uint8_t previousRaw=0;
    static int previousStable=-2,previousAck=-2;
    static uint32_t previousGeneration=0,last=0,changes=0;
    static bool previousQualified=false;
    const uint32_t now=millis();
    const bool qualified=s.qualified(now);
    const int ack=s.acknowledged()?s.acknowledgedCount():-1;
    const bool changed=first || previousRaw!=s.rawMask() || previousStable!=s.stable() ||
        previousAck!=ack || previousGeneration!=s.generation() || previousQualified!=qualified;
    if (changed) ++changes;
    // First physical contact/release as seen by the 100 ms sampler, before the
    // 8/10 vote presents it. RAM-captured and coalesced (generator_trace.h);
    // the RTC/text gates below stay unchanged.
    if (!first && previousRaw!=s.rawMask())
        BlackBoxTrace(GeneratorTrace::WireRaw,
            int32_t(uint32_t(s.rawMask()) | (uint32_t(previousRaw)<<4) |
                (uint32_t(s.stable()+1)<<8) | (uint32_t(s.sampleCount())<<12)),
            int32_t(uint32_t(s.connectedVotes(0)) | (uint32_t(s.connectedVotes(1))<<4) |
                (uint32_t(s.connectedVotes(2))<<8) | (uint32_t(s.connectedVotes(3))<<12)),
            (wireEnabled?1:0) | (qualified?2:0) | (previousGeneration!=s.generation()?4:0));
    previousRaw=s.rawMask(); previousStable=s.stable(); previousAck=ack;
    previousGeneration=s.generation(); previousQualified=qualified;
    if (!first && uint32_t(now-last)<500) return; // At most 2 detailed wire records/sec.
    if (!first && !changes && uint32_t(now-last)<10000) return;
    first=false; last=now;
    const uint8_t a=s.connectedVotes(0),b=s.connectedVotes(1),c=s.connectedVotes(2),d=s.connectedVotes(3);
    uint8_t ambiguous=0;
    if (s.sampleCount()==10) {
      const uint8_t votes[4]={a,b,c,d};
      for (unsigned i=0;i<4;++i) if (votes[i]>2 && votes[i]<8) ambiguous|=1u<<i;
    }
    const uint32_t detail=uint32_t(s.rawMask()) | (uint32_t(a)<<4) | (uint32_t(b)<<8) |
        (uint32_t(c)<<12) | (uint32_t(d)<<16) | (uint32_t(ambiguous)<<20);
    DiagnosticPoint(GeneratorDiagnostic::Wire,s.stable(),detail);
    DiagnosticPrintf("[GD] t=%lu wire raw=%X votes=%u,%u,%u,%u samples=%u ambiguous=%X stable=%d qualified=%u ack=%d gen=%lu age=%lu enabled=%u context=%u retry=%u fence=%u override=%u changes=%lu\r\n",
        (unsigned long)now,s.rawMask(),a,b,c,d,s.sampleCount(),ambiguous,s.stable(),qualified,ack,
        (unsigned long)s.generation(),(unsigned long)s.sampleAge(now),wireEnabled,wireContextValid,
        wireRetryPending,wireFenceNeeded,wireServerCompleted,(unsigned long)changes);
    changes=0;
}

bool WireCompletionContext(GeneratorWireSnapshot &snapshot) {
    if (!wireContextValid || !WireSnapshot().synced() || wireFenceNeeded) return false;
    snapshot=wireContext;return true;
}
void WireRequestContextRefresh() { wireContextValid=false; }

void WireServiceLoop() {
    WireObserveServerSnapshot();
    GeneratorWireState observed = WireSnapshot();
    WireDiagnosticSnapshot(observed);
    WireSampleInputs(false);
    observed = WireSnapshot();
    // Never let boot fencing or a new physical report consume a pending
    // completion token before its authoritative read has reconciled it.
    if (GeneratorNetworkCompletionPending()) return;
    if (!wireSamplerTask || !wireEnabled || !observed.qualified(millis()) ||
        (wireContextValid && observed.synced() && !wireFenceNeeded)) return;
    // Fresh stable values are sent immediately. Only failures are backed off.
    if (wireRetryPending && uint32_t(millis()-wireLastAttempt) < 2000) return;
    char name[32];
    snprintf(name,sizeof(name),"%s",wireObservedName);
    GeneratorWireSnapshot response;
    if (!wireContextValid) {
        const uint32_t generation = observed.generation();
        const GeneratorWireResult result = WireHttpRequest(false,name,0,"",0,response);
        if (result == GeneratorWireResult::Pending) return;
        wireLastAttempt = millis();
        if (result != GeneratorWireResult::Ok || WireSnapshot().generation() != generation || !WireAdoptContext(response)) {
            WireInvalidateAck(); wireContextValid = false; wireRetryPending = true; return;
        }
        observed = WireSnapshot();
        if (response.count == observed.stable() && !wireFenceNeeded) {
            if (!WireAcknowledge(response.count,generation)) {
                wireContextValid = false; wireRetryPending = true; return;
            }
            wireRetryPending = false;
            my["battery_pack"] = response.count; SyncBatteryPackCur();
            Serial.printf("[WireSync] confirmed=%d revision=%lu (read)\n",response.count,(unsigned long)response.revision);
            return;
        }
        wireRetryPending = false;
    }
    observed = WireSnapshot();
    if (!observed.qualified(millis())) return;
    const int sending = observed.stable();
    const uint32_t generation = observed.generation();
    const uint32_t expectedRevision = wireContext.revision;
    char expectedEpoch[37]; memcpy(expectedEpoch,wireContext.epoch,sizeof(expectedEpoch));
    // Uncertain writes (including a previous boot) still require a CAS fence.
    wireFenceNeeded = true;
    const GeneratorWireResult result = WireHttpRequest(true,name,sending,expectedEpoch,expectedRevision,response);
    if (result == GeneratorWireResult::Pending) return;
    wireLastAttempt = millis();
    if (result != GeneratorWireResult::Ok || strcmp(response.epoch,expectedEpoch) != 0 ||
        response.count != sending || response.revision <= expectedRevision ||
        WireSnapshot().generation() != generation || !WireAdoptContext(response)) {
        WireInvalidateAck(); wireContextValid = false; wireRetryPending = true;
        Serial.println("[WireSync] unconfirmed; retry current stable count after resync");
        return;
    }
    if (WireAcknowledge(response.count,generation)) {
        wireFenceNeeded = false; wireRetryPending = false;
        my["battery_pack"] = response.count; SyncBatteryPackCur();
        Serial.printf("[WireSync] confirmed=%d revision=%lu (CAS)\n",response.count,(unsigned long)response.revision);
    } else {
        wireContextValid = false; wireRetryPending = true;
    }
}

void WirePollMain() {
    if (WireCanProgress()) BatteryFinish();
}
// Kept as a compatibility entry point. The main loop services every monitored
// stage before starter progress, rather than doing a second raw theft poll.
void WireTheftMonitorLoop() { WireServiceLoop(); }
