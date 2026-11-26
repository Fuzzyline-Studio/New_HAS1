# HAS1BlackBox

A separate diagnostics library for `iotglove` TTGO, `HAS1_generator`, `HAS1_altar` and the store-3 tag machine (`HAS3_tagmachine`). It does not modify game HTTP, shared HAS2_Wifi, SecureOTA, watchdog/restart policy, partitions, or telnet. Default broker configuration is empty: records stay on the device. No MQTT tasks are created when the broker is empty.

## Operation and limits

`begin(Config{device, firmware, brokerUri, enabled, username, password, largePartition})` creates queues and starts asynchronous discovery. Its return value means initialization was scheduled; `ready()` reports actual partition recovery. `diagnostics()` adds `partitionSize` (0 when no `spiffs` partition exists) and `ringStatus`, the last `Ring::begin` result: 0 unopened, 1 ready, 2 bad size, 3 read error, 4 metadata conflict, 5 foreign data (left untouched, possibly a file), 6 erase error, 7 write error, 8 record conflict. Devices print both on their consoles so a disabled logger can be diagnosed without USB. Events can queue before discovery completes. The valid device names are `iotglove`, `HAS1_generator`, `HAS1_altar` and `HAS3_tagmachine` (one list, `knownDevice()` in `BlackBoxUpload.h`, used by both `begin` and the batch encoder). `largePartition` defaults to `false`; see the raw flash format. Broker URI is an IPv4 literal with explicit port, for example `mqtt://192.168.0.10:1883`. An explicit broker username and password are passed through ESP-MQTT credentials. When MQTT is configured and the username is empty or null, the username defaults to this device’s uppercase 12-hex STA MAC, matching per-device broker ACLs. Flash-only mode leaves an omitted username empty. This fallback never invents a password, enables anonymous broker access, or overrides an explicit username. This initial transport is for a trusted, restricted LAN; it does not implement TLS or cryptographically signed application ACKs. Credentials travel over plain TCP; use an isolated network and broker ACLs. Do not expose the listener to untrusted clients.

`log(type,a,b,c,d,text)` is a non-ISR, zero-wait queue operation. A nonzero return is an accepted ticket, **not a flash durability guarantee**. `flushThrough(ticket,timeoutMs)` checks every earlier accepted ticket and caps waiting at 100 ms. Once any accepted ticket failed, later barriers also report failure; queue-empty is never treated as durability. Producer order and ticket assignment share a short critical section. Tickets and flash sequences stop at their uint32 limits rather than wrapping.

Flash has 1,920 physical record slots. **When full, it recycles the oldest sector, including unacknowledged records, so a device running for weeks can still capture a new fault.** It writes a LOST marker with the overwritten sequence range/count before the new event when possible. A power cut or flash error can also lose that marker; this is a finite rolling diagnostic window, not lossless delivery or first-fault pinning. Capacity is event-rate dependent: 1 record/minute is approximately 32 hours, 1 record/second approximately 32 minutes; periodic library anchors, loss markers, and device events consume capacity. Measure actual rates; these are illustrations, not retention guarantees. Continued logging during a long unrecovered fault eventually overwrites its earliest evidence.

RAM-only queues, records being programmed, and the last uncompleted flash operation may be lost on power removal. Software logging cannot prove a brief supply droop or distinguish all hardware faults. Extract the raw partition offline if the device cannot reconnect. Unknown partitions or interrupted initial formatting are left intact for maintenance, not automatically formatted. The one exception is an empty SPIFFS (format markers only, no file), described under the raw flash format.

## Ownership and interference

- One writer owns all flash operations and the ring state. Event queue: 32 × (64-byte record + 4-byte ticket + 4-byte uptime epoch). Control queue: 8. Snapshot mailbox: one batch of up to 16 records. ACK queue: 8 × 80-byte payload plus length. Ring index uses about 9.6 KiB static RAM.
- Writer stack: 4,096 bytes. Uploader stack: 6,144 bytes plus a 4,096-byte payload buffer allocated once by the uploader (on the stack, 8,192 bytes left only 200–360 bytes in the field). Flash-only builds create neither. ESP-MQTT stack: 4,096 bytes; input/output buffers: 512 bytes each; outbox: 4,096-byte limit. ESP-MQTT itself allocates additional fixed session state. Actual heap/high-water measurements remain a hardware acceptance gate.
- These tasks use priority 1 (same as the Arduino loop on typical builds), unpinned. This is intentionally not described as strictly lower than every game task. Shared radio airtime, flash cache stalls, scheduling, and memory still need benchmarking.
- Producer/game tasks never call MQTT APIs. The uploader owns client start, subscribe, publish, stop, and destroy. Before stop/destroy it waits for any event from `mqtt_task` (all are dispatched after it sets `client->run`; `MQTT_EVENT_BEFORE_CONNECT` is first). If none arrives within 10 s the task cannot be told apart from a starved one, so the client is leaked rather than freed and counted in `upload/stack` v[3]; after three the uploader stops. A session never starts without a registered handler, and a 100 ms `reconnect_timeout_ms` keeps `stop()` after a failed connect short. `esp_mqtt_client_start()` only creates that task; a session cancelled before it ran made `stop()` return without waiting and `destroy()` free the client under it, the field panic `LoadProhibited` in `esp_mqtt_task`/`run_event_loop` with a NULL `client->config`. It publishes with `esp_mqtt_client_publish` from its own task: an `esp_mqtt_client_enqueue` message waits for the MQTT task's next read poll (up to about one second), and a routine game poll in that window cancelled every session before its batch was sent. Callback processing only reassembles at most 80 bytes and queues an ACK. Retained ACKs are rejected.
- `setUploadAllowed(false)` cancels the current generation. `setPaused(true)` also stops new writer work. These are atomic requests and do not wait for quiescence. **An already started flash operation or SDK network operation can finish after the request.** SDK flash serialization still applies during existing OTA; actual OTA/NeoPixel coexistence must be measured. No 20 ms cancellation guarantee is claimed.
- `uploadIdle()` is true when no MQTT client (task, buffers, TCP transport) exists. The uploader marks a session before its last permission check, so after withdrawing permission a caller that sees `uploadIdle()` knows no session can start until permission returns. A client leaked after a task-start timeout is not waited for. The generator waits on it, bounded, before GitHub OTA to return that memory.
- The writer takes the LED/flash guard with zero wait and releases between operations. `tryBeginLedOutput()` also takes it with zero wait, and returns true if the logger never started. The caller must retain pending LED output after failure and only mark refresh complete after `show()` succeeds. Startup partition discovery scans the full partition while holding this guard; it may postpone early LED frames. A one-time erase of an empty SPIFFS (32 sectors, or up to 48 for a 192 KiB partition) extends that startup window once. With `largePartition` the first-use scan reads the whole large partition (about 1.4 MiB for the default scheme) once; the erase stays at the ring's 32 sectors. Once metadata exists no boot reads beyond the ring.
- Writer failures get at most one fresh-slot retry for an uncertain append, then enter a 60-second recovery probe interval. Failed records are not reported durable. No logger activity feeds a game watchdog. There is no logger-triggered reboot.
- There is no diagnostic Wi-Fi scan, automatic radio restart, coredump erase, or NVS dependency in this library. Device integrations report only observations actually available to them.

## Raw flash format (version 1)

Only the existing `spiffs` data partition is used. It must be 131,072 to 196,608 bytes in whole 4 KiB sectors; the ring occupies its first 131,072 bytes. By default larger partitions (for example the default scheme's spiffs) report bad size rather than a long one-time erase. This accepts both the Arduino 3.x `min_spiffs` layout (128 KiB `spiffs` plus `coredump`) and the 2.x layout (192 KiB `spiffs`) that field devices flashed earlier still carry, since OTA never rewrites the partition table. Both metadata sectors precede the data area. A brand-new partition must be entirely `0xff`, or an empty SPIFFS that Arduino formatted there. Such a SPIFFS holds no file: every non-`0xff` byte is the 4-byte tail at offset 252 of a 4 KiB block, the 16-bit `SPIFFS_MAGIC` (`0x20140529 ^ 256 ^ (blockCount - block)`) plus its erase count, and all magics agree on one block count. Both checks cover the **whole** partition, including any part beyond the ring. That partition is erased once sector by sector, verified blank, and then initialized. Any other unrecognized nonblank content disables logging without erasing it. The flash adapter uses `esp_partition_read/write/erase_range`.

**Large partition (opt-in).** `Config::largePartition = true` (passed to `Ring::begin(newId, true)`) additionally accepts a `spiffs` partition above 196,608 bytes, in whole sectors, up to 2 MiB (`Ring::LargeMaxSize`, 0x200000). This is for devices whose partition table cannot change, such as the store-3 tag machine on the Arduino default 4 MB scheme (`spiffs` at 0x290000, 0x170000 bytes = 368 sectors, no `coredump` partition). The ring still occupies only the first 131,072 bytes, and the library never writes or erases anything beyond them. A first initialization still requires the **whole** partition to be unused by the rules above (all `0xff`, or an empty SPIFFS with one consistent block count); anything else is foreign data and nothing is erased. For an empty SPIFFS only the ring's 32 sectors are erased and verified blank; the SPIFFS block magics beyond the ring stay in place. Because erased blocks are skipped and the remaining magics keep their block count, a power cut during that erase, or after it but before the first metadata checkpoint, is still classified as unused on the next boot and initialization finishes. A cut in the middle of one sector erase can leave an intermediate value in that block's 4-byte magic tail, which is then classified as foreign data (as on the small path) and leaves logging disabled until the partition is cleared over USB. A partition that still carries those leftover magics is not a valid SPIFFS any more: firmware using this option must never mount or format SPIFFS on it. Partitions up to 196,608 bytes behave exactly as without the option (whole-partition erase). A ring created with the option is refused with bad size, not reinitialized, if the option is later dropped. `diagnostics().partitionSize` reports the real partition size. `decode_flash.py` accepts such a dump and decodes its first 131,072 bytes.

Metadata is an append-only journal of 128 × 64-byte headers: 64 slots in each sector at offsets 0..8,191. The original two-header layout at offsets 0 and 4,096 remains readable as a sparse journal. Every integer is little endian:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `BBR1` |
| 4 | 4 | format version = 1 |
| 8 | 4 | generation (no wrap) |
| 12 | 8 | random nonzero ring ID |
| 20 | 4 | reserved sequence exclusive upper bound |
| 24 | 4 | durable application ACK frontier |
| 28 | 4 | physical slots = 1920 |
| 32 | 4 | commit marker = 0x434f4d31 |
| 36 | 24 | zero |
| 60 | 4 | CRC32 over bytes 0..59 |

Recovery scans all 128 headers and chooses the highest valid generation. Any valid header with a different ring identity, or equal generation with different bytes, disables the ring. Generation zero and ACK frontiers outside the reserved range are invalid. Updates alternate sectors and program/read-verify the first entirely erased 64-byte slot in the inactive sector. Torn slots are consumed and never rewritten. Only when all 64 slots there are dirty is that inactive sector erased and verified blank; the latest valid generation remains intact in the other sector. Both sectors have a valid header before the first event is appended. Partial first initialization with no valid copy remains disabled rather than guessing a new identity. Sequence numbers reserve blocks of 64 in metadata **before** any event using that block is written. On reboot, unused reservations are skipped. This ensures identities are never reused after torn writes; gaps are normal. NVS erase does not change ring identity.

Events occupy 64-byte slots from offset 8,192 through 131,071:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 1 | marker 0xb7 |
| 1 | 1 | event type, 1..15 |
| 2 | 2 | reserved zero |
| 4 | 4 | sequence, nonzero |
| 8 | 4 | random boot ID |
| 12 | 4 | monotonic uptime milliseconds modulo 2^32 |
| 16 | 16 | four signed int32 values |
| 32 | 28 | ASCII text, max 27 printable bytes + NUL; controls replaced by `?` |
| 60 | 4 | IEEE CRC32 (reflected polynomial 0xedb88320), bytes 0..59 |

Event IDs: 1 Boot, 2 Crash, 3 WifiEvent, 4 WifiTry, 5 WifiSummary, 6 HttpSummary, 7 Recovery, 8 State, 9 Health, 10 Progress, 11 Peer, 12 Logger, 13 Lost, 14 Dropped, 15 Acked. Lost is emitted on unACKed sector recycling; its values are first sequence, last sequence, count, zero (sequence fields interpreted as uint32 bit patterns). Acked is reserved; ACK persistence uses metadata. Device-specific field meanings live in each device's BLACKBOX document. `INT32_MIN` is the unknown sentinel when applicable, not zero voltage/RSSI.

Library-owned Boot values: reset reason, unknown, unknown, unknown; text is firmware version. Dropped values: cumulative count, delta, zero, zero. Logger `uptime64/errors` records once per minute while capacity and writer availability permit: `v[0]` is high 32 bits of uptime in milliseconds, `v[1]` flash errors, `v[2]` upload failures, `v[3]` dropped count. The ordinary uptime field supplies low bits. Wall-clock timestamps cannot be inferred from uptime alone; a missing anchor across an outage can leave rollover ambiguous. Boot IDs identify reset boundaries, not trustworthy calendar time. The additional `erase/lost/stack` Logger record contains erase-call count (including failed attempts), maximum observed erase microseconds, overwritten-record count for this boot, and writer task minimum free stack (ESP-IDF bytes). The `upload/stack` Logger record that follows contains the uploader task's minimum free stack in bytes (0 until its first session), uploaded and failed upload counts, and the number of sessions whose `mqtt_task` never signalled its start within 10 s. These measurements are diagnostics, not enforced latency bounds.

Crash summary. The ESP32 core saves an ELF coredump to the `coredump` partition on a panic or watchdog reset. Without a `coredump` partition (for example the default 4 MB scheme) the image check fails and `crashState` becomes 3 (none); nothing is logged. After ring discovery a one-shot `bb-core` task (own 4 KiB stack) reads its summary and logs `Crash` records describing the previous boot, which the RTC evidence copy skips (`prior.` prefix): `prior.core:<task>` with values exception PC, Xtensa `EXCCAUSE`, `EXCVADDR`, and flags (backtrace depth, `0x100` corrupted, `0x200` fresh, reset reason in bits 16+); `prior.core.bt0`..`bt3` with up to 16 backtrace PCs, four per record; and `prior.core.elf:<9 hex>` with the coredump version and the image's trailing CRC32, which identifies the crash instance. The image is not erased (it stays readable over USB). That CRC is remembered in RTC memory, so one image is logged once per power cycle: a panic or task/interrupt-watchdog reset that wrote a new image is logged as fresh, a kept image after power-on once more as older, and the same image after another reset not again. `diagnostics()` exposes `crashState` (0 not checked yet, 1 fresh, 2 older, 3 none), `crashTask`, `crashPc`, `crashCause` and `uploaderStackFree`; the `upload/stack` Logger record is written only when a broker is configured. Decode PCs with the release's encrypted ELF artifact (see `docs/blackbox/RELEASE.md`), whose ELF SHA256 starts with the logged 9 hex digits: `xtensa-esp32-elf-addr2line -pfiaC -e <sketch>.ino.elf <pc> <bt...>`.

Every append must succeed and match a readback byte-for-byte. A failed write can already have changed flash; dirty slots are never reprogrammed. Recovery scans all slots, rejects duplicate valid sequence numbers or records beyond metadata reservations, ignores CRC-invalid slots, and preserves their dirty status until a safe sector erase. ACKed whole sectors recycle first; when none are available, the oldest sector recycles with the LOST procedure above. Data corruption beyond this model (for example an undetected CRC collision) is not guaranteed recoverable. Raw flash is neither encrypted nor tamper-proof.

Application ACKs first advance a RAM frontier. Metadata checkpoints after 64 sequence positions of progress, at the 60-second writer summary, before reclaiming a sector based on an ACK, and as part of a sequence reservation. Reservation writes occur once per 64 sequences (and after resets that skip reservations). A power cut before checkpoint causes replay; it never allows reclaim based only on a lost RAM ACK. The append-only metadata journal amortizes an erase over 64 checkpoints per sector rather than erasing on every checkpoint. A host endurance test performs 4,161 metadata writes with 64 metadata erases (32 per sector), over 64 times fewer erases than the previous policy. This is an operation-count test, not a hardware lifetime guarantee. The two metadata sectors still wear faster than data sectors. Estimate endurance from actual event/upload rates and flash specifications; continuous high-rate logging is not an intended workload.

## MQTT and durable ACK

MQTT 3.1.1 QoS 1, clean session. Topics:

```
has1/blackbox/v1/AABBCCDDEEFF/batch
has1/blackbox/v1/AABBCCDDEEFF/ack
```

MAC is uppercase 12 hex digits. Batch payload:

```
BB1 iotglove AABBCCDDEEFF 0123456789abcdef 89abcdef
<128 lowercase hex characters encoding one full record>\n
...
```

`HAS1_generator`, `HAS1_altar` and `HAS3_tagmachine` are the other device tokens. Ring is 16 lowercase hex digits; batch token is 8 lowercase hex digits. There are 1..16 records, strictly increasing sequence, maximum payload 4,096 bytes. The header and every record end with an actual LF byte. There are no END lines. Skipped/torn/reserved sequences need not be contiguous.

Collector commits **all** records to SQLite before publishing:

```
ACK1 0123456789abcdef 89abcdef 1234
```

It ends with one LF; last value is canonical unsigned decimal for the final record in this exact batch. The device requires byte-exact ring/token/final-sequence match. MQTT broker PUBACK never advances the journal. The writer alone applies application ACK. A failed checkpoint causes safe replay. SQLite must deduplicate `(MAC, ring, sequence)` and reject different payloads with the same identity. Batch IDs are random correlation values, not authentication secrets.

Normal backlog pacing is 5 seconds. Failed sessions back off with ±20% jitter, capped at a 60-second base. The uploader targets 1,500 ms from snapshot request through ACK wait, with a 250 ms SDK network timeout. **SDK API/stop/destroy latency can exceed that target; it is not a proven wall-clock bound.** A single batch is pending, and a failed/cancelled session destroys the client and bounded RAM outbox. Its flash data remains for the next session. Benchmarks must measure impact, particularly when the broker is absent or Wi-Fi is unstable.

## Tests

```
python3 libraries/HAS1BlackBox/tests/run_tests.py
```

Host tests use C++17 plus AddressSanitizer and UndefinedBehaviorSanitizer on Unix. They cover the CRC vector, byte order/strings, all 65 record/header/metadata-journal-rollover/rolling LOST-marker write cut points, sequence reservations across reboot, ACK checkpoint interruption, unknown data preservation, empty-SPIFFS adoption (including an interrupted erase) versus any file, the opt-in large partition (ring-only erase, nothing beyond the ring written/erased across recycling, interrupted erase recovery, foreign data inside and beyond the ring, size cap, reopen without reading beyond the ring), foreign byte or inconsistent magic left untouched, invalid size/read failure, duplicate records, partial erase, full capacity, LOST ranges, ACK coalescing/replay and recycling, and exact bounded protocol formatting/ACK validation. They exercise the pure storage/protocol implementation, **not FreeRTOS scheduling, real flash power loss, MQTT SDK latency, or target hardware behavior**. Firmware builds and independent device tests are additional gates.

## Evidence build 20261024.2

The `Boot/evidence.20261024.2` record identifies diagnostic schema 2 independently
of the unchanged deployed firmware release number, OTA protocol, partition and
64-byte record format. Its values are actual ESP reset cause, schema 2, replayed
RTC count, and warm-reset flag. `firmware`/`device.boot` remain separate records.

Eight recent critical records are also copied to a fixed RTC no-init journal with
individual CRCs in the zero-wait producer. This adds no flash write or recovery
flush wait. Normal peer/worker summaries and prior RTC re-exports are excluded.
Warm-reset recovery validates slots and queues their **original boot ID and uptime**;
new flash sequence IDs are assigned, so an already persisted event can appear
again. `Logger/rtc.replay.end` gives count, warm flag, `may_duplicate=1`, reserved.
This is a bounded replay marker, not an assertion that every prior event was saved.
Power-on and brownout discard RTC contents. This does **not** add power-loss durability
or pin the first fault indefinitely. The event queue now stores an additional
32-bit uptime-wrap value per record; only the writer publishes sequence coverage.

Optional clock probes use the same authenticated logging session and existing
`batch`/`ack` ACLs. Collector sends exactly `T1 <32 lowercase hex token>\n` on `ack`
after committing a batch and before its normal, unchanged `ACK1`. Device replies
once per session on `batch` with bounded JSON:

```
{"v":1,"kind":"time_probe","mac":"AABBCCDDEEFF","ring":"0123456789abcdef","boot":123,"token":"0123456789abcdef0123456789abcdef","uptime_ms":123456,"seq_min":100,"seq_max":200,"record_uptime_wraps":0}
```

The uptime is sampled while processing this challenge, not at batch creation or
from an old boot in the uploaded backlog. `seq_min..seq_max` covers writer-confirmed
records from the **current boot and one uptime32 wrap epoch**; coverage resets at
wrap and is refused if uptime has crossed to another epoch before a new record
persists. There are no flash reads, scans, JSON parser, new game messages or new
producer waits. Uploader may spend at most another 50 ms after an ordinary ACK
waiting for this optional publish, still inside the existing 1,500 ms session
target, and continues honoring upload/OTA cancellation. SDK stop latency remains
outside a proven bound. Failure or no challenge yields no anchor and never delays
normal application ACK persistence. This is a PC round-trip bracket, **not NTP
synchronization or proof of PC UTC accuracy**. Collector must retain clock quality
as unverified and reject expired/mismatched tokens, wall-clock jumps and excessive
round trips. Old firmware ignores the extra challenge; old collectors ignore the
optional response safely without treating it as a record batch.
