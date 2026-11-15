# Generator networking and conditional completion

## Compatibility and release status

Completion uses the generator-only `CompleteGenerator` API when wire snapshots
advertise `completion_protocol: 1` (server PR #87). A server without it gets one
legacy `Send device_state` per stage, as before v38, so server and firmware may
be deployed in either order. A staff `battery_max` override always finishes with
the legacy Send. See [conditional completion recovery](COMPLETION_RECOVERY.md)
for the path choice, durable intent, deployment and field checks.

An immutable epoch/revision/target is stored before sending. Ambiguous responses
are reconciled with a fresh token-bearing read and retried with the original
condition while it remains valid. Changed conditions abandon that intent and
resynchronize; they never silently renew an old operation onto a new revision.
A durable pending operation survives reboot, and cannot consume its own original
revision through a boot wire SET before reconciliation.

Contributions retain the existing bounded queue and uncertain-delivery policy;
they are not made exactly-once or automatically replayed. Their capture timing
is preserved and transmission has lower priority than control requests.
This is source/PR validation, not a firmware release or store installation.

## What changed

- A single worker owns local HTTP sockets and Wi-Fi connection recovery. Main
  queues requests without waiting, then validates and applies results. Main alone
  modifies `my`, `tag`, `shift_machine` and calls `DataChanged`.
- There are eight fixed request slots and one response mailbox. Optional reports
  leave space for polling/state repair. No per-request task creation or synchronous
  fallback occurs when capacity is exhausted. Unsent contribution retention is
  bounded separately; uncertainty after transmission is never blindly retried.
- Plain numeric-IP GET requests share a 2000ms deadline across connect, write,
  headers and body. This is a configurable bench starting value, not a proven
  best timeout for the current PHP proxy. Header and body sizes are bounded.
  See the transport library's `tests/BOUNDED_TRANSPORT.md` for framing and limits.
- The deadline starts at dispatch, not enqueue. Queue wait is logged separately
  from transport duration; response-consumption wait is not measured separately.
  Offline queued work does not stop the game
  loop. OS socket cleanup may return after its deadline; the result reports that
  overrun, and the worker watchdog remains the final guard against a stuck call.
- Normal polling waits two seconds after completion. Missed ticks are skipped.
  Failures use 2/4/8-second backoff plus up to 200ms jitter. Server heartbeat/offline
  thresholds must be checked before selecting production values.
- Wire sampling retains the one-second startup and symmetric 8/10 qualification.
  CAS epoch/revision validation remains in place. Queued wire values coalesce;
  changed physical/game generations invalidate old work/results.
- Tag lookup is asynchronous and retryable while a card remains present. Old
  card results cannot approve a replacement card. Completion waits for server
  confirmation rather than assuming a queued Send succeeded.
- External Google Sheet uploads are removed from boot/game execution. RTC/Telnet
  diagnostics and queued local crash reports remain. A full queue leaves the
  initial report pending; transport failure is visible without deleting retained
  local evidence.
- Boot no longer erases the entire NVS partition. Initial server state is obtained
  asynchronously before normal game transitions are applied.
- Wi-Fi recovery uses an opt-in asynchronous controller. Legacy `HAS2_Wifi`
  callers retain their existing behavior. Ordinary AP/server unavailability is
  handled as an offline/retry condition, not an immediate reboot instruction.
  Scan buffers are retained until the driver's completion event establishes safe
  ownership. A scan that exceeds six seconds is cancelled; if its completion
  event is still missing four seconds later, a driver fault is latched. Main
  records the fault and, unless the retained own-restart count is non-zero or a
  restart safety gate blocks, restarts at once through the offline-recovery path
  (`wifi.offline.recovery` stage 3, detail 4). Otherwise the worker, which stops
  feeding its own watchdog, causes the reset. Thus a wedged driver still resets
  the device, separately from ordinary connection failures.
- OTA is an explicit maintenance path. It is not included in the promise of
  uninterrupted normal gameplay. Worker and main state ownership still applies.
  Cancelled queued work can drain offline; maintenance aborts after ten seconds
  without connectivity rather than holding the normal queue indefinitely.
- GitHub OTA needs memory the running device does not have spare. The core's
  mbedTLS uses fixed 16 KiB in/out record buffers, about 45 KiB per HTTPS request
  including two ~17 KiB contiguous blocks, while a generator runs with ~48 KiB
  free (largest block 31-45 KiB). Every GitHub request failed at connect (HTTP -1).
  Before `ota.check()` the main task therefore waits (up to 15 seconds after the
  request) for the blackbox MQTT session to end, then stops the BLE controller
  (`Has1BleBeacon::suspend()`). Stopping it alone freed ~20 KiB and the TLS
  session connected, but the update still failed: `Update.begin()` could not get
  its 4 KiB buffer once the download started (`[OTA] OTA 시작 공간 부족`, TLS minimum
  free heap 2.3 KiB). The controller's static memory is therefore released too
  (`releaseMemory()`, about 22 KiB by the ESP-IDF region table, one ~10 KiB
  block; the `[NET] OTA memory` heap/largest before and after shows the real
  gain). A successful update restarts the device. Since BLE can only start again
  at boot, a failed or skipped one also ends in a restart: the main task first
  lets the OTA callback's `device_state` report leave (at most ten seconds),
  records `ota.github.restart` in the blackbox, then restarts. The loop task is off
  its watchdog during the check, so a five-minute `esp_timer` restarts the device
  if SecureOTA ever stalls beyond its own timeouts.
- GitHub maintenance starts only when `device_state` changes to `github`,
  including the first snapshot after boot, so with the server still at `github`
  every restart starts another attempt. A retained RTC counter, incremented just
  before each check, stops this after three attempts: `[NET] GitHub OTA not
  retried` is logged and BLE and gameplay continue. Because the count is taken
  before the check, a panic or watchdog reset inside it is bounded too. Software,
  panic and watchdog resets keep the counter; power-on, brownout and external
  resets, or any `device_state` other than `github`, clear it. A skipped check
  (already up to date) reports `setting`, which clears it after the restart. The local HTTP update
  (`device_state=update`) needs no TLS and keeps its path without these steps, so
  it remains the fallback if the new path misbehaves.

Network diagnostics identify request kind/id/generation, queue delay, transport
phase and elapsed time. The worker publishes a small progress snapshot; main
records it without requiring the worker to format or transmit Telnet output.
Wi-Fi disconnect events likewise publish a protected reason/time/sequence snapshot
for main to consume, without logging from the event callback.
Retained diagnostic evidence is bounded and cannot guarantee preservation of
every event across power loss or a long outage.

## Correctness boundaries

The server CAS protects completion state/count writes, including repeated requests
whose responses were lost. Local generations still only cancel queued work or
ignore stale results; they cannot retract a request already on the network.
Server state changes and process restarts invalidate the original condition.

The server does not provide a durable operation ledger or exactly-once external
audio/EXP follow-up. A snapshot confirms current state, not a historical proof
that all post-commit effects completed. Contribution requests remain legacy.
The device's flash journal follows NVS guarantees, not arbitrary flash-corruption
recovery. Storage failures block new completion mutations and are diagnosed.

Input, audio, LED and OS socket cleanup retain their own timing limitations.
Moving network waits off main does not establish a universal maximum loop time.
Existing HTTP/Wi-Fi/OTA recovery and shared libraries are otherwise unchanged.

## Verification commands

Use ArduinoJson 7.4.3 headers and the pinned Arduino-ESP32 3.3.11 toolchain.

```sh
python3 devices/HAS1_generator/tests/run_poll_schedule_tests.py
python3 libraries/HAS2_Wifi/tests/run_connection_tests.py
SANITIZE=1 python3 libraries/HAS2_Wifi/tests/run_bounded_transport_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_network_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_wire_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_http_body_tests.py
```

Also run the existing tagger, diagnostic, Telnet, DFPlayer and NeoPixel suites.
The network harness executes production worker/facade code with controlled
scheduling. It is not proof of actual FreeRTOS race freedom or RF behavior.
Transport tests include a real loopback socket but do not reproduce the store AP.

Required hardware checks before release: delayed/partial/no server responses,
server and AP outage/recovery, wire changes while requests are pending, tag swap,
new-game transition during an in-flight mutation, explicit employee reboot,
OTA maintenance, repeated startup and worker stack/heap/socket high-water marks.
With the conditional-completion server, test successful completion, lost replies
after commit, never-delivered requests, restart restoration, stale epoch/revision
conflicts and staff recovery. See COMPLETION_RECOVERY.md for the current rollout
checklist; the historical test summary below describes the earlier firmware.

## Historical verification before conditional completion (2026-10-22)

Independent review of the network isolation resolved the scan-buffer race, offline maintenance drain,
stale contribution session, epoch restoration of completed states, tag queue
exhaustion and completion verification after card removal. Compatibility review
also fixed operation-result mixups, completion suppression by a prior echo flag,
queued writes overriding newer server states, and cancellation racing dispatch.
The host suite verifies normal completion, lost ACK, unchanged/error/ignored
responses, same-stage re-entry, definite pre-dispatch retry, staff override and
tagged-stage restoration without introducing automatic mutation retries after a
request byte was written. It also covers the connect-stage resend (added
2026-10-23 after a field case where a completion Send was lost to a 2 s connect
timeout and staff had to force the state): armed only by a fresh start-state
verify, bounded to three attempts, same token, deferred to the next verify while
the queue is full, withdrawn while offline or after five seconds in the queue,
dropped after a staff override or any other authoritative state change,
rejected at dispatch when wires changed, never issued for a previous game
generation, and not mistaken for the device's own echo. With these cases the
network suite is 48 groups (was 37). Remaining known limits: the five-second
queue lifetime is enforced from the main loop, so a main-loop stall (audio
wait, LED blink) extends it by that stall; and when the server leaves the start
state while a resend is merely pending the transaction stays unresolved, while
an already queued resend takes the existing queued-cancel reset.

| Check | Result |
| --- | --- |
| Bounded transport, including real loopback sockets | 48 cases passed, ASan/UBSan |
| Existing-server worker/completion integration | 37 groups passed, ASan/UBSan; includes actual `DataChanged`, `ActivateFunc`, `BatteryFinish` and `StartFinish` |
| Wi-Fi | 13 controller cases plus scan barrier, missing-event fault, callback concurrency and blocking-path guards passed |
| Wire / polling and crash reporting | 36 / 4 groups passed |
| Diagnostic / Telnet / Telnet lifecycle | 10 / 14 / 15 groups passed |
| DFPlayer / NeoPixel / legacy HTTP / tagger | 20 / 8 / 9 groups and tagger suite passed |
| TTGO T1, Arduino-ESP32 3.3.11 | Compile passed: 1,339,533 bytes flash (68%), 90,532 bytes globals (27%) |

The compile used a placeholder signing secret and is not an installable release.
Build memory figures do not measure runtime heap or worker stack headroom.
No firmware was flashed or published. Same-game contribution closure uses the
last sampled gauge count; pulses not yet read from PCNT at the transition are
not guaranteed to be included.
