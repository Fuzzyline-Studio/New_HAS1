"""Read-only decoder for a 128 KiB HAS1BlackBox raw spiffs partition dump.

With --large-partition (firmware built with Config::largePartition) a whole-sector
dump up to 2 MiB is accepted; the ring is its first 128 KiB either way.

This command does not connect to a device, erase a partition, import records, or ACK.
"""
import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

from protocol import EVENT_NAMES, ProtocolError, decode_record


def metadata(raw):
    if len(raw) != 64 or raw[:4] != b"BBR1":
        return None
    version, generation, lo, hi, reserved, acked, slots, commit = struct.unpack_from("<8I", raw, 4)
    if (version != 1 or generation == 0 or slots != 1920 or commit != 0x434F4D31 or not (lo or hi) or
            reserved == 0 or acked >= reserved or
            struct.unpack_from("<I", raw, 60)[0] != zlib.crc32(raw[:60])):
        return None
    return {"ring": f"{(hi << 32) | lo:016x}", "generation": generation,
            "reserved_through_exclusive": reserved, "acked": acked}


LARGE_MAX = 0x200000  # Ring::LargeMaxSize


def decode(path, output, large_partition=False):
    path = Path(path)
    # 128 KiB spiffs, or the Arduino 2.x 192 KiB one whose first 128 KiB holds the ring.
    size = path.stat().st_size
    if large_partition:
        # e.g. the default 4 MB scheme's 0x170000 spiffs on the store-3 tag machine.
        if size < 131072 or size > LARGE_MAX or size % 4096:
            raise ValueError("expected a whole-sector spiffs partition of 131072..2097152 bytes (not whole flash)")
    elif size not in (131072, 196608):
        raise ValueError("expected a 131072 or 196608 byte spiffs partition (not whole flash); "
                         "use --large-partition for a larger one")
    data = path.read_bytes()[:131072]
    # Two 4 KiB sectors hold an append-only journal of 128 64-byte headers.
    # Older firmware with one header at each sector start is still readable.
    valid, damaged_metadata, generations = [], [], {}
    ring = None
    for slot in range(128):
        offset = slot * 64
        raw = data[offset:offset + 64]
        if raw == b"\xff" * 64:
            continue
        entry = metadata(raw)
        if entry is None:
            damaged_metadata.append({"kind": "damaged_metadata", "slot": slot,
                                     "offset": offset, "raw_hex": raw.hex()})
            continue
        if ((ring is not None and ring != entry["ring"]) or
                (entry["generation"] in generations and generations[entry["generation"]] != raw)):
            raise ValueError("ambiguous metadata; preserve dump for manual recovery")
        ring = entry["ring"]
        generations[entry["generation"]] = raw
        valid.append({**entry, "slot": slot, "offset": offset})
    if not valid:
        raise ValueError("no valid BBR1 metadata; preserve dump for manual recovery")
    meta = max(valid, key=lambda entry: entry["generation"])
    if meta["acked"] >= meta["reserved_through_exclusive"]:
        raise ValueError("invalid metadata ACK boundary")
    output.write(json.dumps({"kind": "metadata", **meta, "valid_headers": len(valid),
                             "damaged_headers": len(damaged_metadata)}) + "\n")
    for entry in damaged_metadata:
        output.write(json.dumps(entry) + "\n")
    seen = set()
    valid_records, damaged, erased = 0, 0, 0
    for slot in range(1920):
        offset = 8192 + slot * 64
        raw = data[offset:offset + 64]
        if raw == b"\xff" * 64:
            erased += 1
            continue
        try:
            record = decode_record(raw)
            if record.seq >= meta["reserved_through_exclusive"] or record.seq in seen:
                raise ProtocolError("duplicate or unreserved sequence")
            seen.add(record.seq)
        except ProtocolError as exc:
            damaged += 1
            output.write(json.dumps({"kind": "damaged_slot", "slot": slot, "offset": offset,
                                     "reason": str(exc), "raw_hex": raw.hex()}) + "\n")
            continue
        valid_records += 1
        output.write(json.dumps({"kind": "record", "ring": meta["ring"], "slot": slot,
                                 "offset": offset, "seq": record.seq, "boot": record.boot,
                                 "uptime_ms": record.uptime_ms, "type": record.kind,
                                 "event": EVENT_NAMES[record.kind], "values": record.values,
                                 "text": record.text, "acked": record.seq <= meta["acked"],
                                 "raw_hex": raw.hex()}) + "\n")
    output.write(json.dumps({"kind": "summary", "valid": valid_records, "damaged": damaged,
                             "erased": erased}) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("partition", type=Path)
    parser.add_argument("--large-partition", action="store_true",
                        help="accept a whole-sector dump up to 2 MiB (firmware with Config::largePartition)")
    args = parser.parse_args()
    decode(args.partition, sys.stdout, args.large_partition)


if __name__ == "__main__":
    main()
