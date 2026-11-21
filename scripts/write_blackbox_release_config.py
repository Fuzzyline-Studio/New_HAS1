#!/usr/bin/env python3
"""Generate the existing firmware secret header and public blackbox build metadata.

No credential is printed or included in public metadata. Official glove TTGO /
generator / altar releases require MQTT; local flash-only builds remain supported.
"""

import argparse
import ipaddress
import json
import os
from pathlib import Path
import re

from firmware_targets import TARGETS
from write_firmware_secret import checked_secret

PREFIXES = {"iotglove": "IOTGLOVE", "HAS1_generator": "HAS1_GENERATOR",
            "HAS1_altar": "HAS1_ALTAR"}
PRIVATE_LAN = tuple(ipaddress.ip_network(value) for value in
                    ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


def checked_broker(value):
    # Literal LAN IPv4 only: no DNS, userinfo, paths, query, implicit port or TLS
    # downgrade. These are the same protocol/address limits as the uploader.
    match = re.fullmatch(r"mqtt://([0-9]+(?:\.[0-9]+){3}):([1-9][0-9]{0,4})", value or "")
    if not match:
        raise ValueError("BLACKBOX_BROKER_URI must be mqtt://LAN-IPv4:port")
    try:
        address = ipaddress.IPv4Address(match[1])
    except ipaddress.AddressValueError:
        raise ValueError("BLACKBOX_BROKER_URI must contain a canonical IPv4 address") from None
    if not any(address in network for network in PRIVATE_LAN) or int(match[2]) > 65535:
        raise ValueError("BLACKBOX_BROKER_URI requires a private LAN IPv4 and port 1..65535")
    return value


def checked_password(value):
    if (not value or not value.strip() or len(value.encode("utf-8")) > 127 or
            any(ord(char) < 32 or ord(char) > 126 for char in value)):
        raise ValueError("BLACKBOX_MQTT_PASSWORD requires 1..127 printable ASCII bytes")
    normalized = value.upper()
    if any(marker in normalized for marker in (
            "PLACEHOLDER", "REPLACE_WITH", "CHANGE_THIS", "COMPILE_ONLY",
            "NEVER_RELEASE", "TEST_ONLY", "TEST-ONLY", "YOUR_PASSWORD")):
        raise ValueError("BLACKBOX_MQTT_PASSWORD must not be a test placeholder")
    return value


def configuration(device, mode="auto", broker=None, password=None, *, release=False):
    if device not in TARGETS or mode not in ("auto", "flash", "mqtt"):
        raise ValueError("Unsupported firmware target or blackbox mode")
    if mode == "auto":
        mode = "mqtt" if device in PREFIXES else "flash"
    if release and device in PREFIXES and mode != "mqtt":
        raise ValueError("Official glove/generator/altar releases require MQTT blackbox; flash is local-only")
    if mode == "mqtt" and device not in PREFIXES:
        raise ValueError("MQTT blackbox is supported only for iotglove, HAS1_generator and HAS1_altar")
    uri = checked_broker(broker) if mode == "mqtt" else ""
    credential = checked_password(password) if mode == "mqtt" else ""
    defines = ""
    if device in PREFIXES:
        prefix = PREFIXES[device]
        defines = (f"#define {prefix}_BLACKBOX_BROKER_URI {json.dumps(uri)}\n"
                   f"#define {prefix}_BLACKBOX_MQTT_PASSWORD {json.dumps(credential)}\n")
    public = {"schema": 1, "device": device,
              "mode": mode if device in PREFIXES else "not_applicable",
              "broker_uri": uri, "authentication": "device_mac" if mode == "mqtt" else "none"}
    return defines, public


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, choices=TARGETS)
    parser.add_argument("--mode", default="auto", choices=("auto", "flash", "mqtt"))
    parser.add_argument("--release", action="store_true", help="require MQTT for official glove/generator/altar releases")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--public-config", required=True, type=Path)
    args = parser.parse_args()
    try:
        key = checked_secret(os.environ.get("HMAC_SECRET"))
        defines, public = configuration(args.device, args.mode,
                                       os.environ.get("BLACKBOX_BROKER_URI"),
                                       os.environ.get("BLACKBOX_MQTT_PASSWORD"), release=args.release)
    except ValueError as error:
        parser.error(str(error))
    # Validate everything before replacing the header. Public metadata has an
    # explicit allowlist and never contains a credential or credential digest.
    header = args.root / TARGETS[args.device][0] / "secrets.h"
    header.write_text(f"#pragma once\n#define HMAC_SECRET {json.dumps(key, ensure_ascii=True)}\n" +
                      defines, encoding="utf-8")
    args.public_config.write_text(json.dumps(public, sort_keys=True, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
