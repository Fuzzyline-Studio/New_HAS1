"""Bounded BB1 wire decoder; independent of MQTT and the device runtime."""
from dataclasses import dataclass
import re
import struct
import zlib

MAX_PAYLOAD = 4096
MAX_RECORDS = 16
EVENT_NAMES = {1: "Boot", 2: "Crash", 3: "WifiEvent", 4: "WifiTry", 5: "WifiSummary",
               6: "HttpSummary", 7: "Recovery", 8: "State", 9: "Health", 10: "Progress",
               11: "Peer", 12: "Logger", 13: "Lost", 14: "Dropped", 15: "Acked"}
TOPIC = re.compile(r"has1/blackbox/v1/([0-9A-F]{12})/batch\Z")
HEADER = re.compile(
    r"BB1 (iotglove|HAS1_generator) ([0-9A-F]{12}) ([0-9a-f]{16}) ([0-9a-f]{8})\Z"
)
HEX_RECORD = re.compile(r"[0-9a-fA-F]{128}\Z")


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Record:
    raw: bytes
    kind: int
    seq: int
    boot: int
    uptime_ms: int
    values: tuple
    text: str


@dataclass(frozen=True)
class Batch:
    device: str
    mac: str
    ring: str
    batch: str
    records: tuple

    @property
    def ack_topic(self):
        return f"has1/blackbox/v1/{self.mac}/ack"

    @property
    def ack_payload(self):
        return f"ACK1 {self.ring} {self.batch} {self.records[-1].seq}\n".encode("ascii")


def decode_record(raw):
    if len(raw) != 64 or raw[0] != 0xB7 or raw[1] not in EVENT_NAMES or raw[2:4] != b"\0\0":
        raise ProtocolError("invalid record length/mark/type/reserved bytes")
    if struct.unpack_from("<I", raw, 60)[0] != zlib.crc32(raw[:60]):
        raise ProtocolError("record CRC mismatch")
    seq, boot, uptime = struct.unpack_from("<III", raw, 4)
    if seq == 0:
        raise ProtocolError("record sequence must not be zero")
    return Record(raw, raw[1], seq, boot, uptime,
                  struct.unpack_from("<iiii", raw, 16),
                  raw[32:60].split(b"\0", 1)[0].decode("utf-8", errors="replace"))


def parse_batch(topic, payload):
    topic_match = TOPIC.fullmatch(topic) if isinstance(topic, str) else None
    if topic_match is None or len(payload) > MAX_PAYLOAD:
        raise ProtocolError("invalid topic or oversized payload")
    try:
        text = payload.decode("ascii")
    except (UnicodeDecodeError, AttributeError) as exc:
        raise ProtocolError("payload must be ASCII bytes") from exc
    if not text.endswith("\n"):
        raise ProtocolError("missing terminal newline")
    lines = text[:-1].split("\n")
    if not 2 <= len(lines) <= MAX_RECORDS + 1:
        raise ProtocolError("batch must contain 1..16 records")
    match = HEADER.fullmatch(lines[0])
    if match is None or match[2] != topic_match[1]:
        raise ProtocolError("header/topic identity mismatch")
    records = []
    for line in lines[1:]:
        if HEX_RECORD.fullmatch(line) is None:
            raise ProtocolError("record must contain exactly 128 hex characters")
        record = decode_record(bytes.fromhex(line))
        if records and record.seq <= records[-1].seq:
            raise ProtocolError("sequence must increase strictly within batch")
        records.append(record)
    return Batch(*match.groups(), tuple(records))


def named_values(record, device=None):
    """Named values do not replace original signed wire integers or raw bytes."""
    schema = VALUE_SCHEMAS.get(record.text)
    if record.kind == 3 and record.text.startswith("ap:"):
        schema = (("arduino_event","enum"),("disconnect_reason","enum"),("channel","channel"),("reserved","raw"))
    if schema is None:
        return {"schema": "unknown", "values": list(record.values)}
    fields = {}
    for (name,unit),value in zip(schema,record.values):
        unsigned = unit in ("uint32", "id", "bits", "ipv4", "uptime_ms")
        interpreted = value & 0xffffffff if unsigned or unit == "ms" else value
        unknown = value == -2147483648 and not unsigned
        item = {"value": None if unknown else interpreted, "unit": unit, "raw_signed": value}
        if unknown:
            item["status"] = "unavailable"
        if unit == "ipv4":
            import ipaddress
            item["display"] = str(ipaddress.IPv4Address(interpreted))
        fields[name] = item
    if record.kind == 3 and re.fullmatch(r"ap:[0-9A-F]{12}", record.text):
        fields["ap_bssid"] = {"value": record.text[3:], "unit": "MAC"}
    return {"schema": "BB1:evidence2:"+record.text, "fields": fields}


VALUE_SCHEMAS = {
    # Device-local diagnostic tags; BB1 signed integers and raw bytes stay intact.
    'button.edge': (('press_id','id'),('down','boolean'),('disposition','enum'),('command_sequence','uint32')),
    'button.context': (('press_id','id'),('context_bits','bits'),('queued_events','count'),('in_flight_sequence','uint32')),
    'button.suppressed': (('press_count','count'),('window_ms','ms'),('trace_limit','count'),('reserved','raw')),
    'button.command': (('press_id','id'),('command_sequence','uint32'),('stage','enum'),('result','enum')),
    'motor.applied': (('motor','boolean'),('source','enum'),('press_id','id'),('reserved','raw')),
    'motor.suppressed': (('edge_count','count'),('final_motor','boolean'),('window_ms','ms'),('edge_limit','count')),
    'wifi.offline.recovery': (('stage','enum'),('outage_ms','ms'),('attempts','count'),('detail','uint32')),
    'restart.request': (('cause','enum'),('attempts','uint32'),('reserved_0','raw'),('reserved_1','raw')),
    'restart.wait': (('cause','enum'),('attempts','uint32'),('total_wait_ms','ms'),('remaining_ms','ms')),
    'restart.wait_cancel': (('cause','enum'),('attempts','uint32'),('elapsed_ms','ms'),('reserved','raw')),
    'restart.backoff_reset': (('previous_attempts','uint32'),('healthy_ms','ms'),('worker_sequence','uint32'),('reserved','raw')),
    'primary.failure': (('error','enum'),('http_status','status'),('elapsed_ms','ms'),('semantic_valid','boolean')),
    'primary.recovered': (('error','enum'),('http_status','status'),('elapsed_ms','ms'),('semantic_valid','boolean')),
    'primary.result': (('error','enum'),('http_status','status'),('elapsed_ms','ms'),('semantic_valid','boolean')),

    'rtc.replay.end': (('replayed_count','count'),('warm_reset','boolean'),('replay_may_duplicate','boolean'),('reserved','raw')),
    'operation.detail': (('operation_id','id'),('event','enum'),('phase','enum'),('detail','uint32')),
    'worker.stall': (('network_phase','enum'),('operation_sequence','uint32'),('operation_age_ms','ms'),('progress_age_ms','ms')),
    'wifi.down.threshold': (('down_age_ms','ms'),('connect_failures','count'),('was_up','boolean'),('disconnect_reason','enum')),
    'device.boot': (('actual_reset_reason','enum'),('firmware','version'),('partition','enum'),('training_or_reserved','raw')),
    'output.adc_mv.batt_mv': (('motor','boolean'),('lit_pixels','count'),('adc_pin_mv','mV'),('reported_battery_mv','mV')),
    'prior.rtc.event': (('prior_sequence','uint32'),('event_phase_bits','bits'),('code','raw'),('detail','raw')),
    'prior.rtc.time': (('prior_sequence','uint32'),('prior_uptime_ms','uptime_ms'),('elapsed_ms','ms'),('operation_id','id')),

    'evidence.20261024.2': (('actual_reset_reason', 'enum'), ('evidence_schema', 'version'), ('rtc_replayed_count', 'count'), ('warm_reset', 'boolean')),
    'beetle.log.values': (('peer_boot_id', 'id'), ('peer_uptime_ms', 'uptime_ms'), ('peer_sequence', 'uint32'), ('peer_value', 'uint32')),
    'beetle.log.kind': (('peer_boot_id', 'id'), ('peer_sequence', 'uint32'), ('peer_event', 'enum'), ('peer_code', 'enum')),
    'beetle.hello.state': (('peer_boot_id', 'id'), ('peer_firmware', 'version'), ('peer_partition', 'enum'), ('known_online_bits', 'bits')),
    'beetle.reset.observed': (('observation', 'enum'), ('baseline_boot_id', 'id'), ('observed_boot_id', 'id'), ('reserved', 'raw')),
    'beetle.reset.pulse': (('ttgo_restart_requested', 'boolean'), ('pulse_ms', 'ms'), ('peer_known', 'boolean'), ('baseline_boot_id', 'id')),
    'wifi.ip.gw.mask': (('ip', 'ipv4'), ('gateway', 'ipv4'), ('netmask', 'ipv4'), ('ip_changed', 'boolean')),
    'wifi.no_bssid': (('arduino_event', 'enum'), ('disconnect_reason', 'enum'), ('channel', 'channel'), ('reserved', 'raw')),
    'http.failure': (('operation_id', 'id'), ('error', 'enum'), ('http_status', 'status'), ('elapsed_ms', 'ms')),
    'http.recovered': (('operation_id', 'id'), ('error', 'enum'), ('http_status', 'status'), ('elapsed_ms', 'ms')),
    'http.phase.bytes.errno': (('operation_id', 'id'), ('last_progress_phase', 'enum'), ('received_bytes', 'bytes'), ('errno', 'errno')),
    'http.ok_age.retry.kind': (('operation_id', 'id'), ('last_transport_success_age_ms', 'ms'), ('retry_after_ms', 'ms'), ('request_kind', 'enum')),
    'http.failure.kind': (('request_id', 'id'), ('request_kind', 'enum'), ('error', 'enum'), ('http_status', 'status')),
    'http.recovered.kind': (('request_id', 'id'), ('request_kind', 'enum'), ('error', 'enum'), ('http_status', 'status')),
    'http.phase.errno.body_bytes': (('request_id', 'id'), ('failure_phase', 'enum'), ('socket_errno', 'errno'), ('retained_payload_bytes', 'bytes')),
    'http.queue.total.discard': (('request_id', 'id'), ('queue_ms', 'ms'), ('total_ms', 'ms'), ('discard', 'enum')),
    'http.connect.write.first': (('request_id', 'id'), ('connect_ms', 'ms'), ('write_ms', 'ms'), ('first_byte_ms', 'ms')),
    'http.header.body.close': (('request_id', 'id'), ('header_ms', 'ms'), ('body_ms', 'ms'), ('close_ms', 'ms')),
    'http.ok_age.stale.generation': (('request_id', 'id'), ('last_transport_success_age_ms', 'ms'), ('stale', 'boolean'), ('generation', 'uint32')),
    'worker.queue.gates': (('queue_depth', 'count'), ('flags', 'bits'), ('wifi_status', 'enum'), ('reserved', 'raw')),
    'http.force_reconnect': (('primary_failures', 'count'), ('reconnects', 'count'), ('cooldown_ms', 'ms'), ('reserved', 'raw')),
    'adc.min.age.n.batt_age': (('window_min_adc_pin_mv', 'mV'), ('last_adc_sample_age_ms', 'ms'), ('sample_count', 'count'), ('last_valid_battery_age_ms', 'ms')),
    'output.applied': (('motor', 'boolean'), ('lit_pixels', 'count'), ('brightness_8bit', '0..255'), ('suppressed_output_changes', 'count')),
    'prior.rtc.coverage': (('prior_count', 'count'), ('invalid_count', 'count'), ('not_replayed_count', 'count'), ('warm', 'boolean')),
}
