# Generator reset diagnostics (firmware only)

Use the existing Telnet collector. This change adds `[GD]` lines and `[GD prior]`
replay, and does not connect to devices or change gameplay, timeouts, OTA version,
server reset policy, or the collector. Firmware remains version 26 until release.

## Capturing and interpreting

Keep the collector connected before inserting/removing wires. Keep the entire
file across reconnects, including the first `[diagnostics]`, `[GD prior]`, reset
report, and RAM replay markers. `t` is milliseconds since **that boot**, not wall
clock. Pair records by `id` within a boot; IDs restart after reset. Collector
receive time may differ from event time because history is replayed.

* `BOOT`: numeric ESP-IDF reset reason, firmware, free/minimum heap, accepted prior
  record count. Reset reason identifies the reset mechanism, not its root cause.
* `wire`: observed raw connected mask (bit 0 = ANSWER1; LOW means connected), each
  pin's connected votes out of `samples`, ambiguous mask (3–7/10), stable count,
  current qualification, acknowledged count (`-1` means no ACK), generation,
  age of last sample in ms (`4294967295` means no sample), and sync flags. At most
  twice per second, or every ten seconds when unchanged. `changes` counts main
  observations of changed state, **not every electrical edge**. No sampler logs.
* `wire_ack`: whether the same-generation acknowledgement was accepted. An HTTP
  success alone does not establish that a concurrent local generation accepted it.
* `neo_tx BEGIN/END`: strip in `detail` (0 gauge, 1 starter, 2 inner, 3 circuit).
  END code 0 means ESP32 transmission returned successfully; it is not an LED ACK.
  Negative code is normalized RMT error (generic ESP_FAIL remains -1). Driver
  failure disables that strip until restart; later attempts can return immediately.
* `audio_send`: detail `(folder << 16) | track`. END means the driver call returned,
  **not confirmed playback or an ACK for that particular command**. `audio_drain`
  negative END means pre-play drain timed out. `audio_wait` +1 is a finish event,
  -1 a deadline; battery finish/timeout also appears as a descriptive line.
* `wire_http`: BEGIN detail 0 = read, 1 = CAS write. `get.begin` through `get.end`
  includes DNS, TCP connection, request and HTTP headers together. `get.end` code
  is HTTP status and detail is declared length (4294967295 = unknown). `body.end`
  detail is bytes actually read. `close.begin/end` encloses stop-before-end cleanup.
  END 200 = valid response, 409 = CAS conflict; detail is returned revision.
* `has2_http`: kind is Loop / ReceiveMine / Receive / Send (other is unknown).
  Hook `get.end` includes begin/DNS/TCP/headers. `body.end` codes: 0 OK, 1 timeout,
  2 truncated, 3 connection error, 4 too large, 5 framing error, 6 no memory,
  -1 skipped because transport failed. Detail is **retained decoded body bytes**,
  not bytes on the wire; rejected bodies are cleared. END +1 success, -1 failure.
* `server_watchdog`: a successful Loop response contained a positive server reset
  flag. This records an observation before the existing restart policy runs; it
  distinguishes a requested restart from a hardware/task watchdog reset.
* `crash_upload`: includes existing crash-sheet TLS, redirects and cleanup.
  Positive END is a returned HTTP status below 400; negative is failure. No URL,
  device identity or crash payload is added. This change does not fix its overall
  TLS wait bound; a last unmatched BEGIN makes that path identifiable.
* `HEALTH`: every ten seconds free/minimum/largest heap, Wi-Fi status/RSSI and
  largest loop gap. `loop_gap` reports gaps >=1s after the loop resumes. A frozen
  loop cannot emit health or drain Telnet; inspect warm-reset replay instead.

Wire HTTP negative END reasons: -101 offline/invalid input; -102 HTTP begin
failure; -103 transport/unexpected status; -104 missing/invalid body length;
-105 no stream; -106 incomplete body; -107 invalid JSON/snapshot; -108 rejected
success/status; -109 CAS response disagrees with expected epoch/count/revision.
The HTTP status is available in get/close stages even when END uses a reason code.

## Reset evidence and limits

A 1,040-byte `RTC_NOINIT_ATTR` journal retains the newest **32 compact events**,
not 32 requests or a guaranteed duration. Every risky BEGIN is committed before
the call, even when no Telnet client exists. Entries have checksums and a final
commit write; a torn/corrupt entry is omitted. Header magic/schema/firmware must
match. The journal is recovered after **every** reset reason: field SDIO and
EN-glitch POWERON resets keep RTC memory and are exactly the resets to explain.
A real power loss leaves arbitrary RTC contents, which the header and record
checksums reject; incompatible firmware still discards old contents. `warm` in
`prior.rtc.coverage` and Telnet only reports the reset class (0 = SDIO,
power-on, brownout, EXT, unknown). Its third value now counts events deferred
to the later replay below, not events that are never sent. If a boot dies before `DiagnosticInit()`, the next
boot can see the same older journal again; compare `prior_sequence`/times.
RTC survival is best effort, not persistent storage or a brownout detector.

Blackbox replay: the earliest fault plus newest seven events are logged at boot
as before (Crash). The remaining events follow from the main loop 10 s after
boot as `EventType::Progress` (so reboot evidence tables keep their Wi-Fi and
recovery rows), one event (`prior.rtc.event` + `prior.rtc.time`) per 500 ms,
paused within 300 ms of a DFPlayer call and retried when the 32-entry logger
queue rejects it. `prior.rtc.more.end` closes it (later count, total, warm,
lost time records).

Plug/audio flash trace (`EventType::State`): callers only store to a fixed RAM
buffer (`generator_trace.h`); `BlackBoxLoop()` logs at most one record per loop
and **never within 100 ms of a DFPlayer call**. The blackbox writer's flash
access stalls the other core, which would corrupt the bit-banged 9600-baud
DFPlayer frame or its ACK; nothing is logged next to that exchange. Fourth value:
event age in ms (bits 0-15, saturated) | extra << 16, where extra is the number
of raw changes a `wire.raw` record stands for, or queued records dropped (16
slots) since the previous one. Event time = record uptime - age.
- `wire.raw`: the 100 ms sampler's raw mask changed (raw | previous<<4 |
  (stable+1)<<8 | samples<<12; votes; bit0 enabled, bit1 qualified, bit2
  reset-induced). Coalesced to the latest state, at most one per 250 ms and 30
  per minute; the final state is always logged.
- `wire.present`: count presentation (stable, previous, raw/votes plus bit24
  WirePollMain, bit25 battery audio played). `wire.ack`: server acknowledgement.
- `audio.begin`/`audio.end` around every DFPlayer play; `audio.end` third value
  is ms since the send started (ACK wait for a plain send, until finish/timeout
  for a blocking play; bit1 of the second value = finish received).
RAM entries are lost on reset; the reset moment itself is in the RTC journal
above. Idle heartbeats stay RTC/Telnet-only. Upload policy is unchanged.

At boot, validated records are copied to an independent RAM snapshot before the
new journal starts. Every Telnet connection receives that same previous-boot
snapshot incrementally, even after current boot logs overwrite the 8KiB RAM ring.
A second reset replaces it with evidence from the immediately preceding boot.
A BEGIN without END is a **lead, not proof**: the ring may have wrapped, reset may
have happened between the call returning and END, or the reset may originate in
another task/hardware. POINT `stage` identifies the latest HTTP stage; `kind`
identifies ordinary request type. Wire POINT detail packs raw mask in bits 0–3,
four votes in bits 4–19 and ambiguity in bits 20–23 for periodic snapshots;
wire_ack POINT code +1/-1 carries generation instead. Inspect adjacent text too.

New diagnostic formatting uses a 320-byte fixed stack buffer and only appends to
RAM. Each complete diagnostic line (at most 319 bytes) is appended under one
short lock, preventing another producer from interleaving bytes inside that
line. Legacy Serial/ESP-IDF messages can still interleave across their chunks.
It does not write UART, allocate, write NVS, or call network APIs. Critical
sections contain only fixed record operations; GPIO task never formats/logs.
Normal operation traces are rate limited per category to one per second; slow
(>=250ms) and failed END use a separate one-per-second allowance with `suppressed`
count. The journal still records every compact scope transition. Legacy logs
retain their previous UART behavior, so this is not a claim that all firmware
logging is nonblocking. Existing Telnet send budget remains four nonblocking
256-byte sends per loop. Overflow is explicitly marked; collector connection
cannot guarantee delivery during a freeze or power loss.

텔넷에 보낼 데이터가 남아 있는데 5초 동안 실제 전송이 진행되지 않으면
연결을 정리합니다. 닫기 전에는 기존 루프당 최대 네 번의 전송 예산 안에서
다시 시도해, 읽기를 재개한 수집기는 유지합니다. 기존 수집기는 연결이
종료된 경우 다시 접속하면 됩니다. 입력 데이터는
정체 시간을 연장하지 않으며, 조금씩 전송이 진행되는 느린 연결과 보낼
데이터가 없는 정상 대기는 유지합니다. 이 점검도 메인 루프에서 수행하므로
루프가 멈춘 동안에는 즉시 연결을 정리할 수 없습니다. 새 `[GD]` 로그는
완성된 한 줄 단위로 RAM에 저장하지만 기존 로그의 줄 섞임까지 제거하지는
않습니다. 자세한 수집 동작은 [TELNET_LOGGING.md](TELNET_LOGGING.md)를 참고하세요.

Coverage excludes separate HAS2 async workers, internal Wi-Fi scan stages and
individual RFID calls. Existing crash breadcrumbs remain available for those
paths. This is instrumentation, not proof that electrical faults are absent.
