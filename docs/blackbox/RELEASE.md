# Blackbox firmware release settings

The existing **Deploy Firmware** workflow retains the normal SecureOTA signing,
version increment, fixed release tag and partitions. Select `iotglove` (normal
TTGO glove) or `HAS1_generator`. Training gloves remain a separate USB build;
Beetle and other devices do not gain a blackbox uploader from this workflow.

`blackbox_mode` selects the target's release behavior:

- `auto` (default): resolves to `mqtt` for `iotglove` and `HAS1_generator`;
  existing non-blackbox targets are unchanged.
- `mqtt`: flash journal plus the bounded background MQTT uploader. Requires the
  repository Actions **variable** `BLACKBOX_BROKER_URI` and Actions **secret**
  `BLACKBOX_MQTT_PASSWORD`. Missing/invalid settings fail before version increment
  or publishing. MQTT on unsupported devices fails rather than silently ignoring it.
- `flash`: flash journal only, empty MQTT URI/password. No logs reach Windows
  automatically. This mode is local-build-only for `iotglove` and
  `HAS1_generator`: their official releases require MQTT and reject `flash`.
  Existing non-blackbox targets are unchanged.

For store 1 the intended address is `mqtt://172.30.1.43:1883`; confirm that its
broker, collector, firewall and device accounts are ready before choosing MQTT.
The helper accepts only explicit private LAN IPv4 addresses and ports, with no
DNS, URL credentials or TLS scheme. This matches the existing trusted-LAN uploader.
Firmware uses its uppercase 12-hex-digit Wi-Fi station MAC as MQTT username.
The MQTT client ID is `has1-bb-<MAC>`. The broker must have each device's MAC account and the configured
password, with the supplied per-user topic ACL. The collector uses its separate
account. Changing only the firmware setting does not provision broker accounts.

Passwords must be 1–127 printable ASCII bytes and cannot be compile/test
placeholders. This deployment method provides the selected password to all
firmware instances built from that artifact; the header is ignored by Git and
excluded from source provenance. Like the pre-existing HMAC, the credential is
compiled into the firmware binary, so do not treat binary access as secret-free.

The generated `blackbox-release-config.json` Actions artifact records only the
mode, device, broker address and authentication type. It contains no password,
HMAC, username override or credential hash. The signing key still comes from the
unchanged `HMAC_SECRET` secret and validation. No credentials appear in command
arguments or public build configuration. The normal release assets and OTA
structure remain unchanged.

Every deploy prints the ELF SHA256 in its log and, when the repository secret
`ELF_ARCHIVE_KEY` is set, keeps `firmware-elf-<device>-v<version>-<run_id>`: the
sketch `.ino.elf` and `.map` as `firmware-elf.tar.gz.enc` (AES-256-CBC, PBKDF2,
200,000 iterations, 90 days). Without the secret a warning is logged and no ELF is
kept. This repository is public, so any signed-in GitHub user can download Actions
artifacts; the ELF holds the compiled-in secrets and full symbols, hence only the
encrypted file is uploaded. Keep the key outside GitHub. To decode a crash:

```bash
openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass env:ELF_ARCHIVE_KEY -in firmware-elf.tar.gz.enc | tar -xz
xtensa-esp32-elf-addr2line -pfiaC -e <sketch>.ino.elf <pc> <backtrace pcs...>
```

Match the ELF to a device crash by the 9 hex digits in its `prior.core.elf:` record.

After deployment, verify one idle device produces a stored SQLite batch and ACK
before rolling out broadly. Games/photo/OTA intentionally suppress upload;
absence of a live upload during those periods is not an offline diagnosis.
