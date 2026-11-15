# TTGO blackbox diagnostics

The separate `libraries/HAS1BlackBox` logger records a bounded recent history on
the existing `min_spiffs` data partition. TTGO production and training builds use
the `iotglove` device identifier; `device.boot` v[3] distinguishes training.
Beetle does not link or run the logger. Game HTTP, shared libraries, deployed OTA
protocol and partition table are unchanged. Current automatic restart pacing is
documented in `NETWORK_RECOVERY.md`: the former three-restart cap has been
replaced by continuing retries with backoff. Logging itself does not delay recovery.

## Configuration

`IOTGLOVE_BLACKBOX_ENABLED` defaults to `1`.
`IOTGLOVE_BLACKBOX_BROKER_URI` defaults to `""`: flash only, no MQTT client.
Set `IOTGLOVE_BLACKBOX_MQTT_USERNAME` and `IOTGLOVE_BLACKBOX_MQTT_PASSWORD` in local
`secrets.h` or compiler definitions for a configured broker. Do not put credentials
inside the URI or commit populated secrets. Consult the library and standalone
`tools/windows-blackbox-logger` READMEs for supported transport, broker ACLs and
collector configuration. `--blackbox-mode off|flash|mqtt` supports build checks;
a compile check does not configure a field broker or deploy a device.

An empty/unknown partition is handled by the library's documented initialization
rules. Never erase a used filesystem automatically to make logging work. Field
TTGOs can carry an empty SPIFFS from the older 192 KiB `min_spiffs` layout; it
holds no file and the library erases it once before initializing. TTGOs that still
have that 192 KiB partition table are accepted too: the ring uses the first 128 KiB.

The remote console status (`s`, and on connect) prints `[blackbox] ready status
partition flash_errors uploaded upload_failures dropped upload_stack` and
`[crash] state task pc cause` from the coredump the last panic saved (state 0 =
not checked yet, 1 = this reset, 2 = an older crash, 3 = none; decode per the
library README). `status` is the library
`RingStatus` (1 ready, 2 bad size, 5 foreign data left untouched; see the library
README) and `partition` the spiffs size in bytes, so a silent logger is diagnosable
without USB.

## Device records

Numeric enums refer to the matching firmware source; retain its source archive.
Each row lists v[0..3]; the fixed text field names the subtype. Normal summaries
are emitted once per 60 seconds, first observed failures at most once per 10
seconds, game transitions at most once per second. Wi-Fi connection attempts and
link events are recorded as they occur. Producers enqueue fixed records only.

| Text subtype | Fields |
|---|---|
| `device.boot` | reset reason, firmware, partition version, training flag |
| `game` | phase, role, device state, reserved; `-1` means invalid/unknown |
| `primary.result/failure` | HAS2 HTTP error, HTTP status, elapsed ms, poll valid (`Loop` ok; decoded snapshot valid when `ReceiveMine` was read that cycle) |
| `primary.counts` | primary polls, invalid snapshots, maximum HTTP ms, reserved |
| `network.begin` | worker phase, reserved fields; sampled start, not every request |
| `network.summary` | phase, operation age ms, progress age ms, operation sequence |
| `main.summary` | maximum loop gap ms, loop count, reserved fields |
| `memory.main` | free heap, minimum heap, largest free block, main stack high water |
| `wifi` | connected, RSSI (`INT32_MIN` unavailable), connection failures, last loss reason |
| `recovery.worker_stack` | transport failures, forced reconnects, cooldown ms, worker stack high water |
| `output.adc_mv.batt_mv` | motor, lit pixels, ADC millivolts, calibrated battery millivolts |
| `beetle.online` | peer online flag, reserved fields |
| `link.lost` | disconnect reason, last known RSSI, reserved fields |
| `wifi.event` | Arduino event ID (association, GOT_IP, LOST_IP), reserved fields |
| `connect.begin/end` | failure count/start or connected result/end, failure count/end, reserved fields |
| `loss.context` | last valid phase, role, motor, battery millivolts |
| `loss.output_adc_heap` | motor age ms, lit pixels, raw ADC millivolts, heap |
| `restart.request` | cause (1 server, 2 worker stall, 3 link outage), restart attempts, reserved |
| `prior.link_loss` | retained RTC cause, reason, RSSI, restart attempt |
| `prior.loss_timing` | prior loss/noticed/restart uptime ms, connection failures |
| `prior.loss_context` | prior phase, role, motor, raw ADC millivolts |

ADC/battery fields use `-1` for unavailable/unmeasured; RSSI uses `INT32_MIN`.
These are device field contracts, independent of any collector-wide sentinel.
Live ADC readings preserve measured 0 mV when a sample is known. The legacy RTC
loss record has no validity flag, so retained zero can mean unread or actual zero;
blackbox export conservatively maps that ambiguous value to `-1`. The RTC layout
and its existing diagnostic behavior are unchanged. Raw ADC is not calibrated
battery voltage. Motor age `0xffffffff` in retained context means never observed. Numeric
uint32 counters/timestamps appear as signed int32 fields: reinterpret as unsigned
when calculating wrap-safe durations. A begin without a corresponding summary is
a lead, not proof of which function caused the failure.

## Interference boundaries and limits

Upload is permitted only with fresh known non-active/non-photo game state,
Wi-Fi and an idle/backoff worker. A game operation withdraws permission; it does
not wait for the uploader. MQTT has its own owner, finite memory/backoff and an
application ACK after SQLite commit. MQTT PUBACK alone never marks data as saved.
Independently of successful delivery, a full ring overwrites the oldest unsaved
sector and records a LOST range when possible; this is not lossless storage.
The radio, CPU and flash cache remain shared; no zero-latency promise is made.

LED rendering uses a zero-wait guard against logger flash work. A denied frame is
retried from the current desired output, and the motor-refresh deadline advances
only after an actual show. Motor GPIO updates are not gated. OTA sets a logger
pause flag and retains the existing OTA calls and watchdog exemption. Already
running flash operations cannot be preempted by that flag.

The final restart record is best-effort RAM enqueue with **no wait** once the
current recovery policy, including any repeated-failure backoff, is due; it can be lost. Previously stored records survive according
to the library's flash guarantees. RTC retained loss evidence is copied on warm
boot but is not reliable after power removal.

Typical steady operation generates approximately 10–15 device records per minute
before logger metadata; 1,920 physical slots therefore represent roughly 2–3 hours,
not weeks. This is a rate estimate, not a measured retention guarantee. Failures,
state changes, metadata and slot loss reduce that interval. The ring is a rolling
recent window and does not pin the first fault indefinitely. Successful uploads
retain the longer history on Windows. Sudden power collapse can lose RAM records;
software ADC sampling cannot prove a microsecond supply sag. Offline flash dump
and external voltage measurements remain useful when a device cannot reconnect.

## Validation status

Host policy tests cover unsigned wrap, bounded summary rate and fail-closed upload
conditions. `pixel_refresh_test.cpp` covers denied flash access preserving motor
refresh and post-motor resend requests. OTA dispatch tests assert pause during both
legacy OTA paths and resume on normal/exception return. Firmware compilation is
reported in the implementation verification report, not inferred from host tests.

Pending physical acceptance: logger off / flash only / MQTT, 30 minutes × 3 each;
Wi-Fi loss, broker/collector restart, hard power cut, LED/motor transitions, OTA
and 8-hour soak. Measure p99/max game-poll delay, loop gap, flash erase, heap and
stack high water. Keep telnet sessions at zero. Do not expand deployment before
these gates pass independently on TTGO and generator.

## Evidence schema 2 (20261024.2)

Validated Beetle LOG frames are preserved only after the existing parser and
boot/sequence tracker accept them. `beetle.log.values` stores peer boot ID, peer
uptime ms, peer sequence and value; `beetle.log.kind` stores matching boot/sequence
and the existing diagnostic event/code enums. These records' own boot/uptime are
TTGO **reception** context. The embedded peer uptime is not TTGO time or UTC.
`beetle.hello.state` records peer boot, firmware, partition and known/online flags
(bits 1/2). `beetle.reset.pulse` records TTGO-restart request, pulse length, known
baseline and peer boot. `beetle.reset.observed` records observation enum, baseline
boot and observed boot: a reset request is never treated as a confirmed reset cause.

All existing HTTP completion hooks now capture failure/error change/recovery by
request kind; a side-lane success does not reset a different kind's failure gate.
Types are 0 unknown, 1 own snapshot, 2 unused (was peer read), 3 send, 4 chip report, 5 management.
`http.failure`/`http.recovered`: operation ID, error, HTTP status, elapsed ms.
`http.phase.bytes.errno`: same ID, last result phase, body bytes, unavailable errno
(`INT32_MIN`). `http.ok_age.retry.kind`: same ID, last success age, Retry-After ms,
kind. The unchanged glove HTTP API does not expose socket errno or queue duration;
these are not inferred from an unrelated `errno`. A semantic invalid own snapshot
is separately `primary.failure`, with `primary.recovered` on observed validity.
Changes inside one second are coalesced and the latest pending result is emitted
on the next eligible completion; repeated identical failures summarize at 60 s.
This does not promise every intermediate transition or immediate recovery logging
when no later operation occurs.

`ap:<12 hex BSSID>` records event ID, reason, channel (`INT32_MIN` when not provided),
reserved. `wifi.ip.gw.mask` records IPv4, gateway, netmask canonical big-endian numeric uint32 values and IP-changed flag. Event payloads are copied directly, with no new scans.
`worker.stall` gives phase/operation/operation-age/progress-age; `wifi.down.threshold`
gives down-age/failures/was-up/reason. These are initial fault-threshold records,
not a promise of an immediate reboot during repeated-failure backoff. OTA
exemptions remain; `http.force_reconnect` records the actual policy decision.

`adc.min.age.n.batt_age` gives minimum **ADC pin mV** in the summary window, age of
last raw ADC sample, sample count and age of the last valid calibrated battery
estimate. It reuses only ADC reads already performed by the application. No sample
means `INT32_MIN`, not zero. `output.applied` records actual applied motor state,
lit pixels, brightness8 and number of coalesced output changes; edges are limited
to one record per 500 ms and periodic output summaries still record final state.
These slow samples cannot prove or rule out a fast supply droop. No voltage sensor,
ADC calibration or additional blocking ADC I/O was introduced.

## Button and motor causal evidence (GPIO27)

These observation fields do not change the 30 ms debouncer, role checks, game
queue, haptic priorities, HTTP request order or OTA. Restart pacing is separately
described in `NETWORK_RECOVERY.md`.
`GameModel::buttonPressed` returns the disposition from its actual action branch;
logging does not reimplement the conditions for accepting a press. The same
boot-local `press_id` is copied into the accepted game event and button haptic.
Zero means no known press identity. Join identifiers with the logger's device and
boot ID, never across boots. All uint32 IDs use the existing signed int32 wire
slots: reinterpret the bits as unsigned, including IDs with the high bit set.

| Text subtype | v[0..3] |
|---|---|
| `button.edge` | press ID, debounced down (0/1), disposition, queued command sequence or 0 |
| `button.context` | same press ID, packed state, queue depth after handling, current in-flight command sequence or 0 |
| `button.suppressed` | untraced press count, window ms (10000), press trace limit (4), reserved |
| `button.command` | press ID, command sequence, stage, stage-specific result |
| `motor.applied` | actual applied GPIO level (0/1), active output source, verified active button-pattern press ID or 0, reserved |
| `motor.suppressed` | unrecorded GPIO edge count, final applied level, window ms (1000), edge limit (16) |

Button disposition: 0 release; 1 local Training/Academy acceptance (no button
count write); 2 count command queued; 3 role/state does not accept this button
(including a local non-ghost); 4 unsynchronized; 5 queue full. Queue full keeps the
existing fail-closed resynchronization behavior. `button.context` packed state:
phase in bits 0–7, role in 8–15, device state in 16–23, synchronized in bit 24,
chip present in bit 25. Enums are in `game_state.h`. Unsynchronized context may
contain the last observed server state; it is not a fresh server assertion.

Command stages: 0 admitted to worker queue; 1 start authoritative precheck;
2 enter count write; 3 write call returned; 4 start readback; 5 final worker result;
6 queued command cancelled by connection invalidation. Result is -1 unless it
applies: stage 3 is the existing write-return boolean (0/1), stages 5/6 use
`GameResult::Status` (0 success, 1 unknown, 2 rejected). A write-return boolean is
not independently sufficient to prove server application. A stage begin means
execution reached the existing wrapper, not proof that TCP bytes were sent.
Stage 0 is logged by the main task after nonblocking enqueue; the worker may
record stage 1 before that record. Use IDs and stage meaning, not record receipt
order, to reconstruct the command. Local queued events may also be discarded by
existing state/session reconciliation before submission: `button.edge` without
`button.command` alone is not evidence of a worker freeze. The low-level 3–5 HTTP
operations and server transactions are not individually correlated by this change.

Motor source: 0 no active/known pattern, 1 state pattern, 2 explicit event,
3 operator command, 4 proximity, 5 forced level/mute. Only an active event with a
verified press ID is attributed to the button; chip events have ID 0. Forced
vibration, mute and proximity do not inherit a prior button ID. A pattern's OFF
gap retains its active button ID. Once a pattern expires or is cancelled, OFF
has ID 0: do not infer that a zero-ID OFF identifies which earlier pattern ended.
The initial `motor.applied` is a baseline. Subsequent records represent GPIO
edges, not every pattern/source change while the GPIO remains unchanged.

The first four presses in each fixed 10-second window are traced; their releases
and associated worker command stages are retained when reached, while gameplay
continues for every accepted press. A held button at boot is not a new debounced
press and has no trace identity until released and pressed again. Suppressed
presses are counted and reported at the next loop after the window ends, even if
chatter stops. Motor logging has a separate fixed-window limit of 16 physical
transitions per second (including the initial baseline). Normal Found double
pulses (150 ms ON / 100 ms OFF / 150 ms ON) preserve every edge independent of the
older 500 ms `output.applied` gate. A motor event for an untraced press can still
contain its valid ID; the missing button row is not evidence of a malformed ID.

All producer calls only enqueue fixed numeric records. There is no new flash,
MQTT, socket or ADC call in sensor or motor handling. This is a bounded trace,
not a lossless recorder: the queue or flash ring can still drop/overwrite records,
and power removal can lose RAM data. Burst admission is fixed-window, not a strict
sliding-window rate. Added button activity reduces the approximate retention time
quoted above. Voltage-droop diagnosis still needs external measurement; no ADC
burst sampling or power-sag fix is included here.

Host `button_trace_test.cpp` exercises accepted/ignored/unsynchronized/full queue,
local Academy/Training events, automatic timer event attribution, debounce and
release, noisy input admission and summaries, unsigned timer wrap, all double
pulse edges, forced/muted/proximity source separation, and gameplay waveform/queue
parity with observation metadata enabled or disabled. Physical voltage, device
latency and abrupt-power-loss acceptance tests remain outstanding.


## Continuing restart backoff

The former three-attempt permanent stop and its watchdog-registration inhibition
are removed. The main loop calls the production restart policy each iteration,
independent of one-shot fault-log latches. First thresholds stay 20 seconds for a
normal HTTP worker stall, 30 seconds for an established Wi-Fi link loss and 180
seconds if this boot never connected. After prior recovery restarts, total wait
from the observed continuous fault origin is `max(original threshold, 300/900/1800
seconds)` plus 0–5 seconds jitter. The delay remains capped at 1800 seconds plus
jitter after that; retry count itself does not disable recovery. Actual main-loop
hangs retain their separate 60-second TWDT and cannot honor main-loop backoff.

| Text subtype | v[0..3] |
|---|---|
| `restart.wait` | cause (2 worker/3 link), retained previous attempts, total wait ms, remaining ms at enqueue |
| `restart.wait_cancel` | previous cause, retained previous attempts, elapsed ms since continuous fault origin, reserved |
| `restart.backoff_reset` | previous attempts, healthy interval ms (600000), worker sequence, reserved |
| `restart.request` | cause (1 server/2 worker/3 link), attempts including this request, reserved, reserved |

Wait is logged once when scheduled, cancellation once when no fault qualifies or
OTA protection starts. The origin may be moved earlier when another continuous
fault source is observed, never later merely because the cause changes. A wait
record's remaining value is a sample, not a live countdown. Backoff reset requires
actual Wi-Fi connection and progressing worker for ten minutes; disconnected
retry-loop progress alone does not reset it. Attempts and worker sequences are
uint32 values carried in signed numeric wire slots. Attempt count saturates at
`UINT32_MAX`; the older `LinkLossRecord.restartAttempt` field saturates at 255,
meaning **at least 255**. RTC history magic changes to `GWR3`; this does not change
the separate link-loss record layout or OTA protocol. No new flash persistence is
introduced for restart accounting.
