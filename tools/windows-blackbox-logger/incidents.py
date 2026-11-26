"""Derived game-aware incidents. Never participates in device durable ACKs.

Only fresh, complete server snapshots authorize notifications. MQTT silence and
power cycles are evidence, never an offline rule. Raw telemetry is unchanged.
"""
import argparse
from contextlib import closing
import copy
from datetime import datetime, timedelta, timezone
import json
import logging
from pathlib import Path
import re
import sqlite3
import threading
import time
from types import SimpleNamespace
import uuid

from generator_watch import (ALWAYS_REPORTED, GROUP_MIN, RESET_LABELS, ROLE, SCAN_FAULT, SILENCE_SECONDS, GeneratorWatch,
                             boot_reason, scan_fault_restart)
from incident_evidence import GIVE_UP_SECONDS, summarize, wait_seconds
from telemetry import Event, canonical_json, normalize_mac, parse_utc, utc_now

LOG = logging.getLogger("blackbox")
APP_ID = 0x48424931
FRESH_SECONDS = 30
BATCH = 128
EVIDENCE_DEADLINE = GIVE_UP_SECONDS + 3600  # stop rebuilding a summary that keeps failing
# observed_utc is when the collector received the Boot record, not when the generator booted.
# Older than this when processed means the importer re-read old rows; never announced.
REBOOT_FRESH_SECONDS = 300
# A crash loop while Wi-Fi was down uploads many Boot records at once: one announcement per
# generator per window, the others are in its blackbox table.
REBOOT_QUIET_SECONDS = 600
# A reboot alert still undelivered after this (source down overnight) is no longer news.
REBOOT_DELIVERY_SECONDS = 3600
# A Boot record arrives after the generator is back (upload, then import): a silence that ended
# this recently is the outage that reboot caused.
REBOOT_MERGE_SECONDS = 300
KST = timezone(timedelta(hours=9))


def validate_generators(value):
    if not isinstance(value, dict) or set(value) - {"enabled", "devices", "always", "silence_seconds", "group_min", "role"}:
        raise ValueError("incidents generators contains unknown fields")
    enabled = value.get("enabled", False)
    if type(enabled) is not bool:
        raise ValueError("incidents generators enabled must be boolean")
    # devices: watched during games; always: watched game or not (one no game uses).
    devices, always = value.get("devices", {}), value.get("always", {})
    for group in (devices, always):
        if not isinstance(group, dict) or len(group) > 16:
            raise ValueError("incidents generators devices/always must map up to 16 names to MACs")
        for name, mac in group.items():
            if not isinstance(name, str) or not 0 < len(name) <= 32 or not name.isalnum():
                raise ValueError("incidents generator names must be short alphanumeric")
            normalize_mac(mac)
    if enabled and not (devices or always):
        raise ValueError("incidents generators needs devices or always")
    silence, group = value.get("silence_seconds", SILENCE_SECONDS), value.get("group_min", GROUP_MIN)
    if type(silence) is not int or not 10 <= silence <= 300:
        raise ValueError("incidents generators silence_seconds must be 10..300")
    if type(group) is not int or not 2 <= group <= 16:
        raise ValueError("incidents generators group_min must be 2..16")
    # Shown on cards; store 3 watches its tag machine here with "tagmachine".
    role = value.get("role", ROLE)
    if not isinstance(role, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", role):
        raise ValueError("incidents generators role must be a short lowercase name")
    GeneratorWatch(devices, always=always)  # unique names and MACs across both
    result = {"enabled": enabled, "devices": dict(devices), "always": dict(always),
              "silence_seconds": silence, "group_min": group}
    if "role" in value:
        result["role"] = role
    return result


def validate_config(value, base_path, telemetry_database, device_database, server):
    if not isinstance(value, dict) or set(value) - {"enabled", "database", "notifications", "generators"}:
        raise ValueError("incidents contains unknown fields")
    enabled = value.get("enabled", False)
    if type(enabled) is not bool:
        raise ValueError("incidents enabled must be boolean")
    name = value.get("database", "incidents.sqlite3")
    if not isinstance(name, str) or not name.strip() or len(name) > 1024:
        raise ValueError("incidents database requires a path")
    database = str((Path(base_path) / name).resolve())
    if Path(database) in {Path(telemetry_database).resolve(), Path(device_database).resolve()}:
        raise ValueError("incidents database must be separate from raw databases")
    if enabled and not server.get("enabled"):
        raise ValueError("incidents requires enabled server_jsonl")
    from notifications import validate_config as validate_notifications
    settings = validate_notifications(value.get("notifications", {"enabled": False}), base_path)
    generators = validate_generators(value.get("generators", {"enabled": False}))
    return {"enabled": enabled, "database": database, "notifications": settings, "generators": generators}


def _text(value):
    return value if isinstance(value, str) and 0 < len(value) <= 256 and "\x00" not in value else None


def _open_telemetry(path):
    raw = sqlite3.connect(Path(path).resolve().as_uri()+"?mode=ro", uri=True, timeout=2)
    if raw.execute("PRAGMA application_id").fetchone()[0] != 0x48425431:
        raw.close()
        raise ValueError("not a telemetry database")
    return raw


def _fresh(event, now):
    if not event.occurred_utc:
        return False
    # Rereading old files updates observation but cannot rejuvenate occurrence.
    return all(-2 <= (parse_utc(now) - parse_utc(stamp)).total_seconds() <= FRESH_SECONDS
               for stamp in (event.occurred_utc, event.observed_utc))


class IncidentStore:
    def __init__(self, path, *, readonly=False):
        self.watch = self.watch_source = None
        self.watch_ids = {}  # generator watch key -> incident_id
        self.path = Path(path).resolve()
        if not readonly:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path.as_uri()+"?mode=ro" if readonly else self.path, uri=readonly, timeout=2, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        try:
            if readonly:
                if self.db.execute("PRAGMA application_id").fetchone()[0] != APP_ID or self.db.execute("PRAGMA user_version").fetchone()[0] != 1:
                    raise sqlite3.DatabaseError("not a supported incidents database")
                self.db.execute("PRAGMA query_only=ON")
            else:
                self._initialize()
        except Exception:
            self.db.close()
            raise

    def _initialize(self):
        app, count = self.db.execute("SELECT application_id,(SELECT count(*) FROM sqlite_master WHERE type='table') FROM pragma_application_id").fetchone()
        if app not in (0, APP_ID) or (app == 0 and count):
            self.db.close()
            raise sqlite3.DatabaseError("not an incidents database")
        if self.db.execute("PRAGMA user_version").fetchone()[0] not in (0, 1):
            self.db.close()
            raise sqlite3.DatabaseError("unsupported incidents database")
        mode = self.db.execute("PRAGMA journal_mode=WAL").fetchone()[0]
        self.db.execute("PRAGMA synchronous=FULL")
        if mode.lower() != "wal" or self.db.execute("PRAGMA synchronous").fetchone()[0] != 2:
            raise sqlite3.DatabaseError("required incident WAL/FULL durability unavailable")
        self.db.executescript("""
          BEGIN IMMEDIATE;
          CREATE TABLE IF NOT EXISTS incident_state (
            incident_id TEXT PRIMARY KEY, site_id TEXT NOT NULL, source_id TEXT NOT NULL,
            boot_id TEXT NOT NULL, session_id TEXT NOT NULL, device_id TEXT, mac TEXT,
            state TEXT NOT NULL, reason TEXT, opened_utc TEXT NOT NULL, updated_utc TEXT NOT NULL,
            details TEXT NOT NULL
          );
          CREATE INDEX IF NOT EXISTS incident_source ON incident_state(site_id,source_id,state);
          CREATE TABLE IF NOT EXISTS incident_outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT, incident_id TEXT NOT NULL,
            destination TEXT NOT NULL, version INTEGER NOT NULL, payload TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0,
            next_attempt REAL NOT NULL DEFAULT 0, error_code TEXT, remote_id TEXT,
            UNIQUE(incident_id,destination,version)
          );
          CREATE INDEX IF NOT EXISTS incident_pending ON incident_outbox(state,next_attempt);
          CREATE TABLE IF NOT EXISTS incident_sources (
            site_id TEXT NOT NULL,source_id TEXT NOT NULL,boot_id TEXT,session_id TEXT,
            snapshot_utc TEXT, event_seq INTEGER, validated_run TEXT, deadline REAL,
            active_ids TEXT NOT NULL DEFAULT '[]', health TEXT NOT NULL DEFAULT 'unknown',
            PRIMARY KEY(site_id,source_id)
          );
          CREATE TABLE IF NOT EXISTS incident_meta (key TEXT PRIMARY KEY,value TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS session_links (
            site_id TEXT NOT NULL, session_id TEXT NOT NULL, destination TEXT NOT NULL,
            remote_id TEXT NOT NULL, PRIMARY KEY(site_id,session_id,destination)
          );
          CREATE TABLE IF NOT EXISTS session_times (
            site_id TEXT NOT NULL, source_id TEXT NOT NULL, session_id TEXT NOT NULL,
            started_utc TEXT NOT NULL, ended_utc TEXT, game_group TEXT, theme TEXT,
            PRIMARY KEY(site_id,source_id,session_id)
          );
          CREATE INDEX IF NOT EXISTS session_times_open ON session_times(site_id,source_id) WHERE ended_utc IS NULL;
          CREATE INDEX IF NOT EXISTS session_times_by_session ON session_times(site_id,session_id,started_utc);
          PRAGMA application_id=1212303665;
          PRAGMA user_version=1;
          COMMIT;
        """)

    def close(self):
        self.db.close()

    def attach_generators(self, watch, source_id):
        """Collector-derived generator incidents, filed under the game server they were seen through."""
        self.watch, self.watch_source, self.watch_ids = watch, source_id, {}
        # The watch lives in memory; incidents left open by a previous process resume their recovery.
        for row in self.db.execute("SELECT incident_id,session_id,opened_utc,details FROM incident_state WHERE source_id=? AND state='open' AND reason='generator_silent'", (source_id,)):
            details = json.loads(row["details"])
            try:
                watch.restore(row["incident_id"], details.get("member_macs") or [], parse_utc(details["since_utc"]),
                              row["session_id"], parse_utc(row["opened_utc"]), always=details.get("watch") == "always")
                if row["incident_id"] in watch.open:
                    self.watch_ids[row["incident_id"]] = row["incident_id"]
            except (KeyError, ValueError, TypeError):
                pass  # Ends at the next snapshot like any incident the watch no longer tracks.

    def cursor(self):
        row = self.db.execute("SELECT value FROM incident_meta WHERE key='cursor'").fetchone()
        return int(row[0]) if row else 0

    def start_run(self, run_id):
        # A previous process's monotonic lease is never valid in this process.
        with self.transaction():
            self.db.execute("UPDATE incident_sources SET validated_run=NULL,health='unknown'")
            self.db.execute("UPDATE incident_outbox SET state='manual_review',error_code='interrupted_delivery' WHERE state='sending'")
            self.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('run_id',?)", (run_id,))
            self.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")

    def transaction(self):
        class Transaction:
            def __enter__(inner):
                self.db.execute("BEGIN IMMEDIATE")
            def __exit__(inner, kind, value, tb):
                self.db.execute("ROLLBACK" if kind else "COMMIT")
        return Transaction()

    def invalidate(self, reason="server_stale"):
        self.db.execute("UPDATE incident_sources SET validated_run=NULL,health=?", (reason,))

    def expire(self, run_id, mono):
        self.db.execute("UPDATE incident_sources SET validated_run=NULL,health='server_stale' WHERE validated_run=? AND deadline<?", (run_id, mono))

    def _payload(self, row, status):
        details = json.loads(row["details"])
        role = _text(details.get("role")) or "unknown"
        reason = row["reason"] or "unknown"
        kind = _text(details.get("kind"))
        # Watched devices are generators at store 1; store 3 watches a tag machine the same way.
        watched = role.capitalize()
        if kind == "reboot":
            summary = f"{watched} {row['device_id']} rebooted: reset reason {details.get('reset_reason')} ({details.get('reset_label')}), firmware {details.get('firmware')}. Cause not determined."
        elif kind in ("silence", "group"):
            reboots = details.get("reboots") or []
            rebooted = "; rebooted: " + ", ".join(f"{item.get('name')} ({item.get('label')})" for item in reboots) if reboots else ""
            when = "while always watched" if details.get("watch") == "always" else "during the game"
            summary = f"{watched} {row['device_id']} / {status} / collector rule: no server request for {details.get('gap_ms', 'unknown')} ms {when}; last_seen={details.get('last_seen_utc', 'unknown')}{rebooted}. Cause not determined."
        else:
            summary = f"Device {row['device_id']} / role {role} / {status} / server reason {reason}; last_seen={details.get('last_seen_utc', 'unknown')}; gap_ms={details.get('gap_ms', 'unknown')}. Cause not determined."
        payload = {"incident_id": row["incident_id"], "site_id": row["site_id"],
                   "device_mac": row["mac"], "device_id": row["device_id"],
                   "game_session_id": row["session_id"], "status": status, "summary": summary,
                   "occurred_at": row["opened_utc"] if status in ("offline", "rebooted") else row["updated_utc"],
                   # Structured copies of the summary values for the human-readable Slack card.
                   "role": role, "server_reason": details.get("reason"), "close_reason": row["reason"],
                   "opened_at": row["opened_utc"], "last_seen_utc": details.get("last_seen_utc"),
                   "gap_ms": details.get("gap_ms")}
        # Which game this was, for the Notion session page title.
        times = self._session_time(row["site_id"], row["session_id"])
        payload.update(session_started_utc=times.get("started_utc"), session_group=times.get("game_group"))
        if kind:
            payload.update(kind=kind, members=details.get("members"), reset_label=details.get("reset_label"),
                           firmware=details.get("firmware"))
        return payload, kind

    def current(self, incident_id, queued):
        """A refresh as of now: one retried after the incident recovered must not show it open again."""
        row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not row:
            return queued
        payload, _ = self._payload(row, {"open": "offline", "recovered": "recovered"}.get(row["state"], "monitoring_ended"))
        return dict(payload, refresh=True, **{key: queued[key] for key in ("slack_ts", "notion_page_id", "reboot") if key in queued})

    def _refresh(self, row, channels, reboot):
        """Re-render what was already sent for this incident. Card edits and page updates notify nobody."""
        # Alerts still queued carry the summary from when they were queued.
        for pending in self.db.execute("SELECT id,payload FROM incident_outbox WHERE incident_id=? AND version<=2 AND state IN ('pending','retryable')",
                                       (row["incident_id"],)).fetchall():
            queued = json.loads(pending["payload"])
            queued["summary"] = self._payload(row, queued.get("status"))[0]["summary"]
            self.db.execute("UPDATE incident_outbox SET payload=? WHERE id=?", (canonical_json(queued), pending["id"]))
        payload = self.current(row["incident_id"], {})
        for channel in channels:
            sent = self.db.execute("SELECT remote_id FROM incident_outbox WHERE incident_id=? AND destination=? AND version=1 AND state='delivered'",
                                   (row["incident_id"], channel)).fetchone()
            if not sent or not sent[0]:
                continue  # Not announced yet: the first alert carries the details.
            # Slack also notes the reboot quietly in the card's thread.
            linked = {"slack_ts": sent[0], "reboot": reboot} if channel == "slack" else {"notion_page_id": sent[0]}
            version = max(4, self.db.execute("SELECT coalesce(max(version),0)+1 FROM incident_outbox WHERE incident_id=? AND destination=?",
                                             (row["incident_id"], channel)).fetchone()[0])
            self.db.execute("INSERT INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,?,?,?)",
                            (row["incident_id"], channel, version, canonical_json(dict(payload, **linked))))

    def _queue(self, row, channels, version, status):
        payload, kind = self._payload(row, status)
        for channel in channels:
            linked = {}
            if version > 1:
                delivered = self.db.execute("SELECT remote_id FROM incident_outbox WHERE incident_id=? AND destination=? AND version=1 AND state='delivered'", (row["incident_id"], channel)).fetchone()
                # A bot-posted Slack card has a message ts: its follow-up edits that card and
                # threads under it, so even monitoring_ended closes it out. Webhooks only post recoveries.
                if channel == "slack" and delivered and delivered[0]:
                    linked = {"slack_ts": delivered[0]}
                if channel == "notion" and delivered and delivered[0]:
                    linked = {"notion_page_id": delivered[0]}
                if not delivered or (status != "recovered" and channel != "notion" and not linked):
                    continue
            self.db.execute("INSERT OR IGNORE INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,?,?,?)", (row["incident_id"], channel, version, canonical_json(dict(payload, **linked))))
            # A generator group has no single device MAC, so it has no blackbox table to show.
            if version == 2 and channel == "notion" and linked and kind != "group":
                # Blackbox summary for the event page; built at delivery, once the device backlog has arrived.
                self.db.execute("INSERT OR IGNORE INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,?,3,?)", (row["incident_id"], channel, canonical_json({"incident_id": row["incident_id"], "evidence": True, **linked})))

    def _close(self, incident_id, state, reason, when, channels):
        previous = self.db.execute("SELECT state FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not previous or previous[0] != "open":
            return
        self.db.execute("UPDATE incident_state SET state=?,reason=?,updated_utc=? WHERE incident_id=? AND state='open'", (state, reason, when, incident_id))
        self.db.execute("UPDATE incident_outbox SET state='canceled',error_code='incident_closed_before_delivery' WHERE incident_id=? AND version=1 AND state IN ('pending','retryable')", (incident_id,))
        row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if row:
            self._queue(row, channels, 2, "recovered" if state == "recovered" else "monitoring_ended")

    def _record_open(self, event, item, fields, *, snapshot=False):
        incident = _text(item.get("incident_id"))
        boot, session = _text(fields.get("boot_id")), _text(fields.get("game_session_id"))
        device = _text(item.get("device_id")) or event.device_id
        if not incident or not boot or not session or not device:
            return
        mac = item.get("mac") if snapshot else event.mac
        try:
            mac = normalize_mac(mac)
        except ValueError:
            mac = None  # Never guess MAC from display names.
        opened = item.get("opened_utc") or event.occurred_utc
        try:
            opened = parse_utc(opened).isoformat(timespec="microseconds")
        except (ValueError, TypeError):
            return
        details = {key:fields[key] for key in ("boot_id","game_session_id","game_group","role","reason","last_seen_utc","gap_ms","request_id","request_scope") if key in fields}
        for key in ("role","last_seen_utc"):
            if key in item:
                details[key] = item[key]
        self.db.execute("INSERT OR IGNORE INTO incident_state VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                        (incident, event.site_id, event.source_id, boot, session, device, mac,
                         "open", "server_device_offline", opened, event.occurred_utc, canonical_json(details)))
        if snapshot:
            # A lost/ambiguous inventory binding must remove correlation by MAC,
            # rather than retain a previously plausible but now stale identity.
            self.db.execute("UPDATE incident_state SET mac=?,updated_utc=? WHERE incident_id=? AND site_id=? AND source_id=? AND boot_id=? AND session_id=? AND state='open'", (mac,event.occurred_utc,incident,event.site_id,event.source_id,boot,session))
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident,)).fetchone()
            if row:
                for pending in self.db.execute("SELECT id,payload FROM incident_outbox WHERE incident_id=? AND state IN ('pending','retryable')",(incident,)):
                    payload = json.loads(pending["payload"])
                    payload["device_mac"] = mac
                    self.db.execute("UPDATE incident_outbox SET payload=? WHERE id=?",(canonical_json(payload),pending["id"]))

    def process(self, seq, event, *, now, mono, run_id, channels):
        # The generator watch decides in memory: roll it back with the transaction, so a
        # retried event decides again instead of finding its incident "already open".
        saved = (copy.deepcopy(self.watch), dict(self.watch_ids)) if self.watch else None
        try:
            with self.transaction():
                floor = self.db.execute("SELECT value FROM incident_meta WHERE key='snapshot_floor'").fetchone()
                if event.event_type != "game_monitor_snapshot" or not floor or seq > int(floor[0]):
                    self._process(event, now=now, mono=mono, run_id=run_id, channels=channels)
                self.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor',?)", (str(seq),))
        except Exception:
            if saved:
                self.watch, self.watch_ids = saved
            raise

    def _generator_actions(self, actions, site_id, when, channels):
        for action in actions:
            if action["type"] != "open":
                incident = self.watch_ids.pop(action["key"], None)
                if incident:
                    recovered = action["type"] == "recover"
                    self._close(incident, "recovered" if recovered else "closed",
                                "reconnected" if recovered else action["reason"],
                                action["at"].isoformat(timespec="microseconds") if recovered else when, channels)
                continue
            incident = str(uuid.uuid5(uuid.NAMESPACE_URL, f"has1-generator-silence/{site_id}/{action['key']}"))
            self.watch_ids[action["key"]] = incident
            names, macs = action["names"], action["macs"]
            single = len(names) == 1
            since = action["since"].isoformat(timespec="microseconds")
            opened = action["at"].isoformat(timespec="microseconds")
            last_seen = action["last_seen"].isoformat(timespec="microseconds") if action.get("last_seen") else None
            # An always-watched device is in no game: its outages share the day's card, like reboots outside games.
            session = "게임외-" + action["at"].astimezone(KST).strftime("%Y%m%d") if action["always"] else self.watch.session
            details = {"role": self.watch.role, "kind": "silence" if single else "group", "members": names,
                       "member_macs": macs, "since_utc": since, "last_seen_utc": last_seen, "gap_ms": action["gap_ms"],
                       "boot_id": self.watch.boot, "game_session_id": session, "reason": "generator_silent"}
            if action["always"]:
                details["watch"] = "always"
            inserted = self.db.execute("INSERT OR IGNORE INTO incident_state VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (incident, site_id, self.watch_source, self.watch.boot or "unknown", session,
                 names[0] if single else ("발전기 " if self.watch.role == ROLE else "장치 ") + "·".join(names),
                 macs[0] if single else None,
                 "open", "generator_silent", opened, opened, canonical_json(details))).rowcount
            if inserted:
                self._queue(self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident,)).fetchone(), channels, 1, "offline")

    def _session_times(self, event, fields, session, channels):
        """When each game ran, as its snapshots show it: the server sends no start or end time.

        Read in log order, stale snapshots included, so a collector catching up still sees the start."""
        state = fields.get("game_active")
        active = session if state is True else None
        # Only a complete, well-formed snapshot ends a game; a late line from before it started never does.
        ends = fields.get("snapshot_complete") is True and type(state) is bool and (state is False or session)
        for (ended,) in [] if not ends else self.db.execute("SELECT session_id FROM session_times WHERE site_id=? AND source_id=? AND ended_utc IS NULL "
                                        "AND session_id IS NOT ? AND started_utc<?", (event.site_id, event.source_id, active, event.occurred_utc)).fetchall():
            self.db.execute("UPDATE session_times SET ended_utc=? WHERE site_id=? AND source_id=? AND session_id=?",
                            (event.occurred_utc, event.site_id, event.source_id, ended))
            # A card whose last edit came before the game ended would never show the end.
            card = self.db.execute("SELECT i.* FROM incident_state i JOIN incident_outbox o USING(incident_id) WHERE i.site_id=? AND i.session_id=? "
                                   "AND o.destination='slack' AND o.version=1 AND o.state='delivered' AND o.remote_id IS NOT NULL "
                                   "ORDER BY i.opened_utc DESC LIMIT 1", (event.site_id, ended)).fetchone()
            if card and "slack" in channels:
                self._refresh(card, ["slack"], None)
        if active:
            self.db.execute("INSERT INTO session_times(site_id,source_id,session_id,started_utc,game_group,theme) VALUES (?,?,?,?,?,?) "
                            "ON CONFLICT(site_id,source_id,session_id) DO UPDATE SET started_utc=min(started_utc,excluded.started_utc), "
                            # Still running after a recorded end: that end was wrong.
                            "ended_utc=CASE WHEN ended_utc<excluded.started_utc THEN NULL ELSE ended_utc END, "
                            "game_group=coalesce(game_group,excluded.game_group), theme=coalesce(theme,excluded.theme)",
                            (event.site_id, event.source_id, active, event.occurred_utc, _text(fields.get("game_group")), _text(fields.get("theme"))))

    def _session_time(self, site_id, session_id):
        row = self.db.execute("SELECT started_utc,ended_utc,game_group,theme FROM session_times WHERE site_id=? AND session_id=? "
                              "ORDER BY started_utc LIMIT 1", (site_id, session_id)).fetchone()
        return {"started_utc": row[0], "ended_utc": row[1], "game_group": row[2], "theme": row[3]} if row else {}

    def _merge_reboot(self, event, found, observed, device_boot, channels):
        """A reboot during this generator's silence (still open, or ended within minutes) caused that
        outage: one event, so note it on that line instead of a second line and blackbox table."""
        stamp = observed.isoformat(timespec="microseconds")
        since = (observed - timedelta(seconds=REBOOT_MERGE_SECONDS)).isoformat(timespec="microseconds")
        name = self.watch.names[event.mac]
        # Only an outage that is or will be announced: one whose alert was canceled or failed would hide the reboot.
        for row in self.db.execute("SELECT * FROM incident_state i WHERE site_id=? AND source_id=? AND opened_utc<=? "
                                   "AND (state='open' OR updated_utc>=?) AND json_extract(details,'$.kind') IN ('silence','group') "
                                   "AND EXISTS(SELECT 1 FROM incident_outbox o WHERE o.incident_id=i.incident_id AND o.version=1 "
                                   "AND o.state IN ('pending','retryable','sending','delivered')) "
                                   "ORDER BY opened_utc DESC", (event.site_id, self.watch_source, stamp, since)).fetchall():
            details = json.loads(row["details"])
            if event.mac not in (details.get("member_macs") or []):
                continue
            if details.get("kind") == "group" and found[0] in ALWAYS_REPORTED:
                return False  # a crash in a shared outage is news of its own, with its blackbox table
            reboots = details.get("reboots") or []
            if any(item.get("name") == name for item in reboots):
                return False  # its second reboot is news of its own
            entry = {"name": name, "label": RESET_LABELS.get(found[0], f"reset_{found[0]}"), "firmware": found[1],
                     "device_boot": device_boot, "observed_utc": stamp}
            details["reboots"] = reboots + [entry]
            self.db.execute("UPDATE incident_state SET details=? WHERE incident_id=?", (canonical_json(details), row["incident_id"]))
            # Its card and page were usually sent before this upload arrived: edit them.
            self._refresh(self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (row["incident_id"],)).fetchone(), channels, entry)
            return True
        return False

    def _generator_boot(self, event, now, channels):
        """A crash reboot is over by the time its Boot record arrives: announced once, never left open."""
        record = {"event_type": event.event_type, "fields": event.fields}
        found = boot_reason(record) if self.watch else None
        # A restart after a Wi-Fi scan that never completed is announced from its own record:
        # its Boot record is an ordinary SW restart, which stays unannounced.
        fault = bool(self.watch) and not found and scan_fault_restart(record)
        if fault:
            found = (3, None)
        if not found or event.mac not in self.watch.names:
            return
        age = (parse_utc(now) - parse_utc(event.observed_utc)).total_seconds()
        if not -2 <= age <= REBOOT_FRESH_SECONDS:
            return
        observed = parse_utc(event.observed_utc)
        device_boot = event.fields.get("boot")
        # The SW Boot record of the same restart is what gets noted on an outage line.
        if not fault and self._merge_reboot(event, found, observed, device_boot, channels):
            return
        label = SCAN_FAULT if fault else self.watch.reboot_label(found[0], event.mac)
        if not label:
            return
        # Outside a game the day's reboots share one card and one Notion page; so do the
        # always-watched devices' (the academy's AG), which no watched game uses.
        game = self.watch.session if event.mac in self.watch.watched else None
        session = game or "게임외-" + observed.astimezone(KST).strftime("%Y%m%d")
        quiet = (observed - timedelta(seconds=REBOOT_QUIET_SECONDS)).isoformat(timespec="microseconds")
        # Within one session only: a reboot once a game starts is news again.
        if self.db.execute("SELECT 1 FROM incident_state WHERE reason='generator_reboot' AND site_id=? AND mac=? AND session_id=? AND opened_utc>=?",
                           (event.site_id, event.mac, session, quiet)).fetchone():
            return
        details = {"role": self.watch.role, "kind": "reboot", "reset_reason": found[0], "reset_label": label,
                   "firmware": found[1], "device_boot": device_boot, "game_session_id": session,
                   # The blackbox table is anchored on this record, not on the upload's arrival time.
                   "device_ring": event.fields.get("ring"), "device_seq": event.fields.get("seq")}
        incident = str(uuid.uuid5(uuid.NAMESPACE_URL, f"has1-generator-{'scan-fault' if fault else 'reboot'}/{event.site_id}/{event.mac}/{device_boot}"))
        stamp = observed.isoformat(timespec="microseconds")
        inserted = self.db.execute("INSERT OR IGNORE INTO incident_state VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (incident, event.site_id, self.watch_source, self.watch.boot or "unknown", session,
             self.watch.names[event.mac], event.mac, "recovered", "generator_reboot", stamp, stamp,
             canonical_json(details))).rowcount
        if inserted:
            self._queue(self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident,)).fetchone(), channels, 1, "rebooted")

    def _process(self, event, *, now, mono, run_id, channels):
        if event.source == "device":
            self._generator_boot(event, now, channels)
            return
        if event.source != "game_server":
            return
        fields = event.fields
        boot, session = _text(fields.get("boot_id")), _text(fields.get("game_session_id"))
        incident = _text(fields.get("incident_id"))
        if self.watch and event.source_id == self.watch_source and event.occurred_utc:
            at = parse_utc(event.occurred_utc)
            actions = self.watch.server(boot, at)
            if event.event_type == "request_ingress":
                actions += self.watch.request(event.mac, at)
            self._generator_actions(actions, event.site_id, event.occurred_utc, channels)
        if event.event_type == "device_offline":
            self._record_open(event, {"incident_id": incident}, fields)
            return  # No notification until a complete fresh snapshot confirms it.
        if event.event_type in ("device_recovered", "alarm_cleared"):
            if not incident or not boot or not session or not event.occurred_utc:
                return
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=? AND site_id=? AND source_id=? AND boot_id=? AND session_id=?", (incident,event.site_id,event.source_id,boot,session)).fetchone()
            if row:
                recovered = event.event_type == "device_recovered" and fields.get("reason") == "reconnected" and _fresh(event, now)
                self._close(incident, "recovered" if recovered else "closed", _text(fields.get("reason")) or "unknown", event.occurred_utc, channels)
            return
        if event.event_type == "game_monitor_snapshot" and event.occurred_utc:
            self._session_times(event, fields, session, channels)
        if event.event_type != "game_monitor_snapshot" or not boot or not _fresh(event, now):
            return
        sequence = fields.get("event_seq")
        if type(sequence) is not int or sequence < 0:
            return
        previous = self.db.execute("SELECT * FROM incident_sources WHERE site_id=? AND source_id=?", (event.site_id,event.source_id)).fetchone()
        if previous and (previous["snapshot_utc"] > event.occurred_utc or (previous["boot_id"] == boot and previous["event_seq"] >= sequence)):
            return
        # A newer incomplete snapshot revokes the older lease immediately.
        self.db.execute("INSERT OR REPLACE INTO incident_sources VALUES (?,?,?,?,?,?,?,?,?,?)", (event.site_id,event.source_id,boot,session,event.occurred_utc,sequence,None,None,"[]","invalid_snapshot"))
        # Oversized, partial or malformed snapshots fail closed: no inferred
        # recovery and no alarms from incomplete device inventory.
        active, participants = fields.get("active_incidents"), fields.get("participants")
        if fields.get("snapshot_complete") is not True or type(fields.get("game_active")) is not bool or not isinstance(active, list) or len(active) > 64 or not isinstance(participants, list) or len(participants) > 64:
            return
        if any(not isinstance(item, dict) or not _text(item.get("incident_id")) or not _text(item.get("device_id")) for item in active):
            return
        if any(not isinstance(item, dict) or not _text(item.get("device_id")) for item in participants):
            return
        inventory = {item["device_id"]:item for item in participants}
        if len(inventory) != len(participants) or len({item["incident_id"] for item in active}) != len(active):
            return
        if any(item["device_id"] not in inventory or inventory[item["device_id"]].get("incident_id") != item["incident_id"] for item in active):
            return
        active_by_device = {item["device_id"]:item for item in active}
        if len(active_by_device) != len(active):
            return
        if any(item.get("incident_id") is not None and (item["device_id"] not in active_by_device or active_by_device[item["device_id"]]["incident_id"] != item["incident_id"]) for item in participants):
            return
        try:
            macs = [normalize_mac(item.get("mac")) for item in participants]
            if len([mac for mac in macs if mac]) != len({mac for mac in macs if mac}):
                return
            if any(normalize_mac(item.get("mac")) != normalize_mac(inventory[item["device_id"]].get("mac")) for item in active):
                return
        except ValueError:
            return
        if not fields["game_active"] and (active or participants):
            return
        if fields["game_active"] and not session:
            return
        ids = [item["incident_id"] for item in active] if fields["game_active"] else []
        age = max(0, (parse_utc(now)-parse_utc(event.occurred_utc)).total_seconds(), (parse_utc(now)-parse_utc(event.observed_utc)).total_seconds())
        self.db.execute("INSERT OR REPLACE INTO incident_sources VALUES (?,?,?,?,?,?,?,?,?,?)", (event.site_id,event.source_id,boot,session,event.occurred_utc,sequence,run_id,mono+FRESH_SECONDS-age,canonical_json(ids),"healthy"))
        if self.watch and event.source_id == self.watch_source:
            self._generator_actions(self.watch.game(fields["game_active"], session, parse_utc(event.occurred_utc)),
                                    event.site_id, event.occurred_utc, channels)
        # The new snapshot also reconciles missed end events and process restarts.
        rows = self.db.execute("SELECT * FROM incident_state WHERE site_id=? AND source_id=? AND state='open'", (event.site_id,event.source_id))
        for row in rows:
            if row["reason"] == "generator_silent":
                # Not in the server's list by design: the generator watch ends it with the game.
                # One it no longer tracks (watch disabled, unreadable details) ends here instead; an
                # always-watched one has no game to end with, so it ends as soon as nothing watches it.
                tracked = self.watch and row["incident_id"] in self.watch_ids.values()
                try:
                    always = json.loads(row["details"]).get("watch") == "always"
                except (ValueError, AttributeError):
                    always = False
                if not tracked and always:
                    self._close(row["incident_id"], "closed", "watch_removed", event.occurred_utc, channels)
                elif not tracked and (row["boot_id"] != boot or row["session_id"] != session or not fields["game_active"]):
                    self._close(row["incident_id"], "closed", "game_ended", event.occurred_utc, channels)
                continue
            if row["boot_id"] != boot or row["session_id"] != session or row["incident_id"] not in ids:
                self._close(row["incident_id"], "closed", "snapshot_no_longer_active", event.occurred_utc, channels)
        for item in active if fields["game_active"] else []:
            self._record_open(event, item, fields, snapshot=True)
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=? AND site_id=? AND source_id=? AND boot_id=? AND session_id=? AND state='open'", (item["incident_id"],event.site_id,event.source_id,boot,session)).fetchone()
            if row:
                self._queue(row, channels, 1, "offline")

    def claim(self, *, run_id, mono, now_epoch):
        with self.transaction():
            ready = self.db.execute("SELECT value FROM incident_meta WHERE key='delivery_run'").fetchone()
            if not ready or ready[0] != run_id:
                return None
            # Filter eligibility in SQL BEFORE LIMIT. A previous source's stale
            # queue must never starve another source's currently eligible row.
            # A reboot alert still unsent long after the fact is no longer news (all at once, not one per cycle).
            stale = datetime.fromtimestamp(now_epoch - REBOOT_DELIVERY_SECONDS, timezone.utc).isoformat(timespec="microseconds")
            self.db.execute("UPDATE incident_outbox SET state='canceled',error_code='stale_reboot' WHERE version=1 AND state IN ('pending','retryable') "
                            "AND incident_id IN (SELECT incident_id FROM incident_state WHERE reason='generator_reboot' AND opened_utc<?)", (stale,))
            # A first alert needs the incident in the server's current list. The collector's own
            # generator silences need only the same server boot and game (an always-watched one, only
            # to be still open); a generator reboot is already over and needs only a healthy source.
            rows = self.db.execute("""SELECT o.*,i.site_id,i.source_id,i.boot_id,i.session_id,i.state AS incident_state,i.reason AS incident_reason,
                json_extract(i.details,'$.watch') AS incident_watch
              FROM incident_outbox o JOIN incident_state i USING(incident_id)
              JOIN incident_sources s ON s.site_id=i.site_id AND s.source_id=i.source_id
              WHERE o.state IN ('pending','retryable') AND o.next_attempt<=?
                AND s.validated_run=? AND s.deadline>=?
                AND (o.version>1 OR i.reason='generator_reboot'
                  OR (i.state='open' AND i.reason='generator_silent' AND json_extract(i.details,'$.watch')='always')
                  OR (i.state='open' AND s.boot_id=i.boot_id AND s.session_id=i.session_id
                  AND (i.reason='generator_silent' OR EXISTS(SELECT 1 FROM json_each(s.active_ids) WHERE value=i.incident_id))))
              ORDER BY o.id LIMIT 1""", (now_epoch,run_id,mono)).fetchall()
            for row in rows:
                source = self.db.execute("SELECT * FROM incident_sources WHERE site_id=? AND source_id=?", (row["site_id"],row["source_id"])).fetchone()
                if not source or source["validated_run"] != run_id or source["deadline"] < mono:
                    continue
                listed = row["incident_reason"] == "generator_silent" or row["incident_id"] in json.loads(source["active_ids"])
                same_game = source["boot_id"] == row["boot_id"] and source["session_id"] == row["session_id"]
                if row["version"] == 1 and row["incident_reason"] != "generator_reboot" and (row["incident_state"] != "open" or not listed
                                                                                              or not (same_game or row["incident_watch"] == "always")):
                    continue
                self.db.execute("UPDATE incident_outbox SET state='sending',attempts=attempts+1 WHERE id=?", (row["id"],))
                result = dict(row)
                result["payload"] = json.loads(row["payload"])
                return result
        return None

    def finish(self, delivery_id, result, *, now_epoch, channels, on_delivered=None):
        state = {"delivered":"delivered", "retryable":"retryable", "permanent":"permanent", "unknown":"manual_review"}.get(result.state, "manual_review")
        with self.transaction():
            row = self.db.execute("SELECT * FROM incident_outbox WHERE id=? AND state='sending'", (delivery_id,)).fetchone()
            if not row:
                return
            if state == "retryable" and row["attempts"] >= 12:
                state = "manual_review"
            delay = max(min(3600, 2 ** min(row["attempts"], 10)), min(86400, max(0, result.retry_after)))
            self.db.execute("UPDATE incident_outbox SET state=?,next_attempt=?,error_code=?,remote_id=? WHERE id=?", (state,now_epoch+delay,result.code,result.remote_id,delivery_id))
            incident = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (row["incident_id"],)).fetchone()
            parent = getattr(result, "parent_id", None)
            if parent:
                # The game session's Slack card / Notion page, kept even when this delivery's
                # own write failed so its retry and later events reuse it.
                self.db.execute("INSERT OR REPLACE INTO session_links VALUES (?,?,?,?)", (incident["site_id"], incident["session_id"], row["destination"], parent))
            if on_delivered and state == "delivered":
                on_delivered()  # same transaction: a follow-up cannot be lost between the two writes
            if row["version"] == 1 and state == "delivered" and result.remote_id:
                # Reboots noted while this alert was on its way skipped this channel: edit it now.
                noted = json.loads(incident["details"]).get("reboots") or []
                for entry in noted if row["destination"] == "slack" else noted[-1:]:
                    self._refresh(incident, [row["destination"]], entry)
            if row["version"] == 1 and incident["reason"] == "generator_reboot":
                if state == "delivered" and row["destination"] == "notion" and result.remote_id:
                    # No follow-up exists for a reboot; its page gets the blackbox table (crash records) directly.
                    self.db.execute("INSERT OR IGNORE INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,?,3,?)",
                                    (row["incident_id"], "notion", canonical_json({"incident_id": row["incident_id"], "evidence": True, "notion_page_id": result.remote_id})))
            elif row["version"] == 1 and incident["state"] != "open":
                if state == "retryable":
                    self.db.execute("UPDATE incident_outbox SET state='canceled' WHERE id=?", (delivery_id,))
                elif state == "delivered":
                    self._queue(incident, channels, 2, "recovered" if incident["state"] == "recovered" else "monitoring_ended")

    def queue_slack_evidence(self, incident_id, highlights):
        """After the Notion blackbox table lands, thread a link to it under the session's Slack card."""
        # Only for a device announced on that card: there is no card to thread under otherwise.
        announced = self.db.execute("SELECT 1 FROM incident_outbox WHERE incident_id=? AND destination='slack' AND version=1 AND state='delivered'", (incident_id,)).fetchone()
        row = self.db.execute("SELECT device_id FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not announced or not row:
            return
        payload = {"incident_id": incident_id, "evidence_notice": True, "device_id": row["device_id"],
                   "highlights": [str(item)[:120] for item in highlights or [] if isinstance(item, str)][:4]}
        self.db.execute("INSERT OR IGNORE INTO incident_outbox(incident_id,destination,version,payload) VALUES (?,'slack',3,?)", (incident_id, canonical_json(payload)))

    def slack_session(self, incident_id):
        """Context for the game session's Slack card: its ts, Notion links and every notified device."""
        row = self.db.execute("SELECT site_id,session_id FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not row:
            return None
        links = dict(self.db.execute("SELECT destination,remote_id FROM session_links WHERE site_id=? AND session_id=?", (row["site_id"], row["session_id"])).fetchall())
        event = self.db.execute("SELECT remote_id FROM incident_outbox WHERE incident_id=? AND destination='notion' AND version=1 AND state='delivered'", (incident_id,)).fetchone()
        # Only incidents already announced on Slack (plus the one being delivered): one whose alert was
        # canceled never gets a follow-up, so listing it would leave the card stale.
        announced = ("FROM incident_state i WHERE site_id=? AND session_id=? AND (incident_id=? OR EXISTS("
                     "SELECT 1 FROM incident_outbox o WHERE o.incident_id=i.incident_id AND o.destination='slack' "
                     "AND o.version=1 AND o.state='delivered'))")
        params = (row["site_id"], row["session_id"], incident_id)
        # Unresolved first, then newest: a flapping session must not hide the device that is down now.
        incidents = [dict(item) for item in self.db.execute(
            "SELECT incident_id,device_id,state,opened_utc,updated_utc,reason,json_extract(details,'$.reboots') AS reboots " + announced +
            " ORDER BY state='open' DESC,opened_utc DESC,incident_id LIMIT 64", params)]
        counts = dict(self.db.execute("SELECT state,count(*) " + announced + " GROUP BY state", params).fetchall())
        return {"site_id": row["site_id"], "session_id": row["session_id"], "ts": links.get("slack"),
                "notion_page": links.get("notion"), "event_page": event[0] if event else None,
                "incident_id": incident_id, "incidents": incidents, "counts": counts,
                **self._session_time(row["site_id"], row["session_id"])}

    def defer(self, delivery_id, *, now_epoch, delay):
        # Waiting for late device uploads sends nothing, so it is not a delivery attempt.
        self.db.execute("UPDATE incident_outbox SET state='pending',attempts=attempts-1,next_attempt=? WHERE id=? AND state='sending'", (now_epoch+delay, delivery_id))

    def evidence(self, incident_id, telemetry_database):
        with closing(_open_telemetry(telemetry_database)) as raw:
            row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
            if not row:
                raise ValueError("unknown incident")
            return summarize(raw, row, utc_now())

    def export(self, incident_id, telemetry_database=None, limit=200):
        if type(limit) is not int or not 1 <= limit <= 1000:
            raise ValueError("limit must be 1..1000")
        row = self.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (incident_id,)).fetchone()
        if not row:
            raise ValueError("unknown incident")
        source = self.db.execute("SELECT health,snapshot_utc,boot_id,session_id FROM incident_sources WHERE site_id=? AND source_id=?", (row["site_id"],row["source_id"])).fetchone()
        result = {"incident": dict(row), "source_health":dict(source) if source else {"health":"unknown"}, "notifications": [dict(item) for item in self.db.execute("SELECT id,destination,version,state,attempts,error_code,remote_id FROM incident_outbox WHERE incident_id=? ORDER BY id", (incident_id,))], "related_events": [], "root_cause": "not_determined"}
        result["incident"]["details"] = json.loads(result["incident"]["details"])
        if telemetry_database:
            with closing(_open_telemetry(telemetry_database)) as raw:
                start = (parse_utc(row["opened_utc"])-timedelta(seconds=60)).isoformat(timespec="microseconds")
                end = (parse_utc(row["updated_utc"])+timedelta(seconds=60)).isoformat(timespec="microseconds")
                result["evidence_window"] = {"start_utc":start,"end_utc":end,"padding_seconds":60,
                    "unknown_device_time_rule":"same MAC only; upload arrival never substitutes for occurrence"}
                # Device flash timestamps cannot be derived from upload arrival.
                # Unknown time stays unknown and is separately labeled.
                candidates = raw.execute("SELECT e.payload FROM event_times t JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE t.site_id=? AND ((t.source='game_server' AND t.source_id=? AND t.occurred_utc BETWEEN ? AND ? AND json_extract(e.payload,'$.fields.incident_id')=?) OR (t.mac=? AND (t.occurred_utc BETWEEN ? AND ? OR t.occurred_utc IS NULL))) ORDER BY t.occurred_utc IS NULL,t.occurred_utc LIMIT ?", (row["site_id"],row["source_id"],start,end,incident_id,row["mac"],start,end,limit+1))
                for item in candidates:
                    event = json.loads(item[0])
                    fields = event.get("fields", {})
                    exact = fields.get("incident_id") == incident_id
                    same_mac = bool(row["mac"] and event.get("mac") == row["mac"])
                    if not exact and not same_mac:
                        continue
                    unknown = event["source"] == "device" or not event.get("occurred_utc")
                    result["related_events"].append({"event":event,"relation":"same_incident" if exact else "candidate", "basis":"explicit_incident_id" if exact else "same_mac_time_unknown" if unknown else "same_mac_time_window", "time_uncertainty":"unknown" if unknown or event.get("uncertainty_ms") is None else event["uncertainty_ms"], "causality":"not_established"})
                result["truncated"] = len(result["related_events"]) > limit
                result["related_events"] = result["related_events"][:limit]
        return result

    def list_incidents(self, limit=200):
        if type(limit) is not int or not 1 <= limit <= 1000:
            raise ValueError("limit must be 1..1000")
        return [dict(row) for row in self.db.execute("SELECT incident_id,site_id,source_id,device_id,mac,session_id,state,reason,opened_utc,updated_utc FROM incident_state ORDER BY updated_utc DESC,incident_id LIMIT ?",(limit,))]

    def statistics(self):
        return {"cursor":self.cursor(),
                "incidents_by_state":dict(self.db.execute("SELECT state,count(*) FROM incident_state GROUP BY state")),
                "notifications_by_state":dict(self.db.execute("SELECT state,count(*) FROM incident_outbox GROUP BY state")),
                "sources":[dict(row) for row in self.db.execute("SELECT site_id,source_id,health AS last_recorded_health,snapshot_utc FROM incident_sources ORDER BY site_id,source_id LIMIT 1000")],
                "note":"Stored status only; use running collector health to establish current liveness. No exactly-once external delivery guarantee."}


class IncidentRuntime:
    def __init__(self, config, *, telemetry_database, site_id, source_id, check_disk=lambda: None):
        self.config, self.telemetry_database = config, telemetry_database
        self.site_id, self.source_id, self.check_disk = site_id, source_id, check_disk
        self.run_id = uuid.uuid4().hex
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self.authorized = threading.Event()
        self.threads = []
        self.counts = {"process_errors":0,"notification_errors":0}
        self.health = "awaiting_snapshot"
        notification = config.get("notifications", {})
        # Bot mode keeps one Slack card per game session; Notion goes first so the card can link its page.
        self.slack_cards = bool(notification.get("enabled") and notification.get("slack",{}).get("enabled") and notification.get("slack",{}).get("channel_id"))
        order = ("notion","slack") if self.slack_cards else ("slack","notion")
        self.channels = [name for name in order if notification.get("enabled") and notification.get(name,{}).get("enabled")]
        self.generators = config.get("generators") or {"enabled": False}

    def _new_store(self):
        state = IncidentStore(self.config["database"])
        state.start_run(self.run_id)
        self._watch_generators(state)
        return state

    def _watch_generators(self, state):
        if self.generators.get("enabled"):
            state.attach_generators(GeneratorWatch(self.generators["devices"], always=self.generators.get("always"),
                silence_seconds=self.generators["silence_seconds"], group_min=self.generators["group_min"],
                role=self.generators.get("role", ROLE)), self.source_id)

    def _routed(self, event, state):
        """Events the incident store processes; the cursor skips the rest. Device records
        (source_id is the MAC) reach only the generator reboot rule."""
        if event.site_id != self.site_id:
            return False
        return event.source_id == self.source_id or (event.source == "device" and state.watch is not None)

    def start(self):
        for target,name in ((self._process_loop,"incident-state"),(self._notification_loop,"incident-delivery")):
            thread = threading.Thread(target=target, name=name, daemon=True)
            self.threads.append(thread)
            thread.start()

    def _process_loop(self):
        state = raw = None
        previous_wall, previous_mono = utc_now(), time.monotonic()
        try:
            while not self.stop_event.is_set():
                self.authorized.clear()
                try:
                    self.check_disk()
                    if state is None:
                        state = self._new_store()
                    if raw is None:
                        raw = _open_telemetry(self.telemetry_database)
                    self.ready.set()
                    now, mono = utc_now(), time.monotonic()
                    highwater = raw.execute("SELECT coalesce(max(seq),0) FROM telemetry_ingest").fetchone()[0]
                    state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")
                    step = (parse_utc(now)-parse_utc(previous_wall)).total_seconds()-(mono-previous_mono)
                    if abs(step)>1:
                        state.invalidate("clock_step")
                        state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('snapshot_floor',?)", (str(highwater),))
                    previous_wall, previous_mono = now, mono
                    state.expire(self.run_id,mono)
                    rows = raw.execute("SELECT j.seq,e.payload FROM telemetry_ingest j JOIN telemetry_events e USING(site_id,source,source_id,event_id) WHERE j.seq>? ORDER BY j.seq LIMIT ?", (state.cursor(), BATCH)).fetchall()
                    for seq, payload in rows:
                        event = Event.from_dict(json.loads(payload))
                        # Cursor advances over unrelated sources without treating
                        # other stores/server identities as authorities.
                        if not self._routed(event, state):
                            with state.transaction():
                                state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('cursor',?)",(str(seq),))
                        else:
                            state.process(seq,event,now=now,mono=mono,run_id=self.run_id,channels=self.channels)
                    if state.cursor() >= highwater:
                        state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run',?)", (self.run_id,))
                        self.authorized.set()
                    source = state.db.execute("SELECT health FROM incident_sources WHERE site_id=? AND source_id=?",(self.site_id,self.source_id)).fetchone()
                    self.health = source[0] if source else "awaiting_snapshot"
                except Exception:
                    self.authorized.clear()
                    if state:
                        try:
                            state.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','')")
                            state.invalidate("processing_error")
                        except sqlite3.Error:
                            pass  # The shared Event also denies the delivery worker.
                    # Publish completion only after the durable revocation was
                    # attempted. The in-memory gate closes immediately above.
                    self.health = "processing_error"
                    self.counts["process_errors"] += 1
                    if self.counts["process_errors"] % 120 == 1:
                        LOG.warning("Incident processing unavailable; raw collection remains active", exc_info=True)
                self.stop_event.wait(0.1 if 'rows' in locals() and len(rows)==BATCH else 0.5)
        finally:
            self.authorized.clear()
            if state:
                state.invalidate("collector_stopped")
                state.close()
            if raw:
                raw.close()

    def _notification_loop(self):
        state = sender = raw = None
        try:
            while not self.stop_event.is_set():
                if not self.ready.wait(0.5):
                    continue
                if not self.authorized.wait(0.5):
                    continue
                try:
                    row = None
                    self.check_disk()
                    if state is None:
                        state = IncidentStore(self.config["database"])
                    if sender is None:
                        from notifications import NotificationSender
                        sender = NotificationSender(self.config.get("notifications", {"enabled":False}))
                    row = state.claim(run_id=self.run_id,mono=time.monotonic(),now_epoch=time.time())
                    if row:
                        if not self.authorized.is_set():
                            state.db.execute("UPDATE incident_outbox SET state='pending',attempts=attempts-1 WHERE id=? AND state='sending'", (row["id"],))
                            continue
                        payload = row["payload"]
                        if payload.get("evidence") is True:
                            incident = None
                            try:
                                if raw is None:
                                    raw = _open_telemetry(self.telemetry_database)
                                incident = state.db.execute("SELECT * FROM incident_state WHERE incident_id=?", (row["incident_id"],)).fetchone()
                                now = utc_now()
                                delay = wait_seconds(raw, incident, now)
                                if not delay:
                                    payload = dict(payload, evidence=summarize(raw, incident, now))
                            except Exception:
                                # Nothing has been sent, so this is not an ambiguous delivery: rebuild later.
                                delay = 60
                                self.counts["notification_errors"] += 1
                                if self.counts["notification_errors"] % 120 == 1:
                                    LOG.warning("Blackbox evidence summary unavailable; will retry")
                                if raw:
                                    raw.close()
                                    raw = None
                                if incident and (parse_utc(utc_now())-parse_utc(incident["updated_utc"])).total_seconds() > EVIDENCE_DEADLINE:
                                    state.finish(row["id"],SimpleNamespace(state="permanent",retry_after=0,code="evidence_build_failed",remote_id=None),now_epoch=time.time(),channels=self.channels)
                                    continue
                            if delay:
                                state.defer(row["id"], now_epoch=time.time(), delay=delay)
                                continue
                        if payload.get("refresh") is True:
                            payload = state.current(row["incident_id"], payload)
                        if row["destination"] == "slack" and self.slack_cards:
                            payload = dict(payload, slack_session=state.slack_session(row["incident_id"]))
                        result = sender.send(row["destination"],payload)
                        followup = None
                        if self.slack_cards and isinstance(payload.get("evidence"), dict):
                            highlights = payload["evidence"].get("highlights")
                            followup = lambda: state.queue_slack_evidence(row["incident_id"], highlights)
                        state.finish(row["id"],result,now_epoch=time.time(),channels=self.channels,on_delivered=followup)
                except Exception:
                    # An unexpected exception after claim is ambiguous; never
                    # automatically resend a possibly accepted external request.
                    self.counts["notification_errors"] += 1
                    if state and row:
                        try:
                            state.db.execute("UPDATE incident_outbox SET state='manual_review',error_code='unexpected_delivery_error' WHERE id=? AND state='sending'",(row["id"],))
                        except sqlite3.Error:
                            pass  # Restart also converts any remaining sending rows.
                    if self.counts["notification_errors"] % 120 == 1:
                        LOG.warning("Incident notification unavailable; raw collection remains active")
                self.stop_event.wait(0.5)
        finally:
            if state:
                state.close()
            if raw:
                raw.close()

    def stats(self):
        return {**self.counts,"source_health":self.health,"workers":{thread.name:thread.is_alive() for thread in self.threads}}

    def stop(self, timeout=5):
        self.stop_event.set()
        end = time.monotonic()+timeout
        for thread in self.threads:
            thread.join(max(0,end-time.monotonic()))
        return all(not thread.is_alive() for thread in self.threads)


def main():
    parser = argparse.ArgumentParser(description="Export one derived incident and bounded related raw evidence")
    parser.add_argument("--database", required=True)
    parser.add_argument("--telemetry-database")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--incident-id")
    mode.add_argument("--list", action="store_true")
    mode.add_argument("--stats", action="store_true")
    mode.add_argument("--evidence-summary", metavar="INCIDENT_ID", help="preview the blackbox table a Notion event page receives")
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()
    if args.evidence_summary and not args.telemetry_database:
        parser.error("--evidence-summary requires --telemetry-database")
    store = IncidentStore(args.database, readonly=True)
    try:
        if args.evidence_summary:
            result = store.evidence(args.evidence_summary, args.telemetry_database)
        else:
            result = store.statistics() if args.stats else store.list_incidents(args.limit) if args.list else store.export(args.incident_id,args.telemetry_database,args.limit)
        print(json.dumps(result,ensure_ascii=False,indent=2))
    finally:
        store.close()


if __name__ == "__main__":
    main()
