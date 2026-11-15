"""Generator silence and crash-reboot detection from data the collector already has.

The game server records every generator request, and generators poll Loop every
2 s whether or not a game runs (failures back off to about 8 s). A generator that
sends nothing for 20 s during a game is reported. The watch clock is the newest
request from ANY device, so a stalled server or telemetry log stops the clock
instead of making every generator look silent; after such a pause, silence is
measured again from the moment requests resume. Several generators going silent
together are reported as one group, since they share a cause (seen as one Wi-Fi
drop of all five in Omada DHCP logs). A generator no game uses (the academy's AG) can
be watched always instead: its silence is reported on its own, game or not, and neither a
game end nor a server restart ends it.

Crash reboots recover within seconds and leave no request gap; they come from the
generator's own blackbox Boot record. This module only decides; the incident
store persists and notifies. It holds no clock of its own, so replaying a log
yields the same decisions.
"""
import argparse
from contextlib import closing
from datetime import timedelta
import json
from pathlib import Path
import sqlite3

from telemetry import normalize_mac, parse_utc

SILENCE_SECONDS = 20
GROUP_MIN = 3
GROUP_WINDOW_SECONDS = 10
# With 30+ devices polling every 1-2 s, no request at all for this long means the
# server or its telemetry stalled, not a device.
SERVER_PAUSE_SECONDS = 5
# esp_reset_reason_t values from the generator's device.boot record.
ALWAYS_REPORTED = {4: "panic", 5: "int_wdt", 6: "task_wdt", 7: "wdt", 9: "brownout", 14: "pwr_glitch", 15: "cpu_lockup"}
IN_GAME_REPORTED = {1: "poweron", 2: "ext"}
# Every reason a reboot noted on an outage can carry, including the unannounced SW restart.
RESET_LABELS = {**IN_GAME_REPORTED, 3: "sw", **ALWAYS_REPORTED}


class GeneratorWatch:
    def __init__(self, generators, *, always=None, silence_seconds=SILENCE_SECONDS, group_min=GROUP_MIN,
                 group_window_seconds=GROUP_WINDOW_SECONDS, pause_seconds=SERVER_PAUSE_SECONDS):
        always = always or {}
        everything = list(generators.items()) + list(always.items())
        self.names = {normalize_mac(mac): name for name, mac in everything}
        if len(self.names) != len(everything) or len(set(self.names.values())) != len(self.names):
            raise ValueError("generator names and MACs must be unique")
        self.watched = {normalize_mac(mac) for mac in generators.values()}
        # Watched whether or not a game runs (the academy's AG, which no game uses), each on its own.
        self.always = {normalize_mac(mac) for mac in always.values()}
        self.first = None     # first request seen: an always-watched device silent since then counts from it
        self.silence = timedelta(seconds=silence_seconds)
        # Never wider than half the silence, or healthy generators would join a single one's group.
        self.window = timedelta(seconds=min(group_window_seconds, silence_seconds / 2))
        self.pause = timedelta(seconds=pause_seconds)
        self.group_min = group_min
        self.last = {}        # MAC -> newest request time
        self.clock = None     # newest request time from any device
        self.resumed = None   # end of the latest server-wide pause
        self.boot = None      # game server boot id
        self.boot_since = None  # first event time of that boot
        self.session = None   # game session being watched; None outside games
        self.started = None   # when watching of this session began
        self.open = {}        # key -> {"macs": [...], "since", "opened": datetime, "back": set(), "always": bool}

    def _baseline(self, mac):
        start = self.first if mac in self.always else self.started
        times = [stamp for stamp in (self.last.get(mac), start, self.resumed) if stamp]
        return max(times)

    def _end_all(self, reason):
        """End the game's outages; an always-watched device's outage does not depend on the game or server."""
        ended = sorted(key for key, item in self.open.items() if not item["always"])
        for key in ended:
            del self.open[key]
        return [{"type": "end", "key": key, "reason": reason} for key in ended]

    def restore(self, key, macs, since, session, opened=None, always=False):
        """Re-adopt an incident still open in the store after a collector restart.

        Its game is adopted too: the next snapshot of that same game must not end it."""
        devices = self.always if always else self.watched
        macs = sorted((mac for mac in (normalize_mac(item) for item in macs) if mac in devices), key=lambda mac: self.names[mac])
        if macs and session:
            self.open[key] = {"macs": macs, "since": since, "opened": opened or since, "back": set(), "always": always}
            if self.session is None and not always:
                self.session, self.started = session, since

    def server(self, boot, at):
        """Every game_server event carries its boot id; a new boot restarts measurement."""
        if not boot or boot == self.boot:
            return []
        if self.boot_since and at < self.boot_since:
            return []  # a late line from the previous boot's log, not a restart
        previous, self.boot, self.boot_since = self.boot, boot, at
        self.resumed = max(self.resumed or at, at)
        return self._end_all("server_restarted") if previous is not None else []

    def game(self, active, session, at):
        """A validated server snapshot: start, keep or stop watching a game session."""
        if active and session:
            if session == self.session:
                return []
            actions = self._end_all("game_ended")
            self.session, self.started = session, at
            return actions
        if self.session is None:
            return []
        self.session = self.started = None
        return self._end_all("game_ended")

    def request(self, mac, at):
        """One request_ingress from any device, in log order."""
        if self.clock and at - self.clock > self.pause:
            self.resumed = at
        if self.clock is None or at > self.clock:
            self.clock = at
        self.first = self.first or at
        actions = []
        mac = normalize_mac(mac) if mac else None
        if mac in self.names:
            if mac not in self.last or at > self.last[mac]:
                self.last[mac] = at
            for key, item in list(self.open.items()):
                # A late line stamped before the outage was reported proves nothing about now
                # (a group's `since` is its earliest member's, so compare with the report time).
                if mac in item["macs"] and at > item["opened"]:
                    item["back"].add(mac)
                    if item["back"] >= set(item["macs"]):
                        del self.open[key]
                        actions.append({"type": "recover", "key": key, "at": at})
        return actions + self.evaluate()

    def evaluate(self):
        if self.clock is None:
            return []
        # A group member that came back is watched again while the rest are still out.
        busy = {mac for item in self.open.values() for mac in set(item["macs"]) - item["back"]}
        by_name = lambda mac: self.names[mac]  # noqa: E731
        groups = []
        if self.session is not None:
            silent = {mac: self.clock - self._baseline(mac) for mac in self.watched if mac not in busy}
            over = sorted((mac for mac, gap in silent.items() if gap >= self.silence), key=by_name)
            # Generators that dropped together cross the threshold within a second or two
            # of each other: report the ones close behind in the same group.
            near = sorted((mac for mac, gap in silent.items() if gap >= self.silence - self.window), key=by_name)
            if over:
                groups += [(near, False)] if len(near) >= self.group_min else [([mac], False) for mac in over]
        groups += [([mac], True) for mac in sorted(self.always - busy, key=by_name) if self.clock - self._baseline(mac) >= self.silence]
        actions = []
        for macs, always in groups:
            since = min(self._baseline(mac) for mac in macs)
            key = "/".join([self.boot or "-", "always" if always else self.session, ",".join(macs), since.isoformat()])
            self.open[key] = {"macs": macs, "since": since, "opened": self.clock, "back": set(), "always": always}
            # `since` can be the game start or a server resume; the card shows the real last request.
            seen = [self.last[mac] for mac in macs if mac in self.last]
            actions.append({"type": "open", "key": key, "macs": macs, "names": [self.names[mac] for mac in macs],
                            "since": since, "at": self.clock, "last_seen": min(seen) if seen else None,
                            "gap_ms": int((self.clock - since).total_seconds() * 1000), "always": always})
        return actions

    def reboot_label(self, reason, mac=None):
        """The reset reason worth a notification, or None (OTA and self-recovery restarts are expected).

        A power-on matters while the device is watched: during a game it is used in, or always."""
        if reason in ALWAYS_REPORTED:
            return ALWAYS_REPORTED[reason]
        mac = normalize_mac(mac) if mac else None
        watched = mac in self.always or (self.session is not None and (mac is None or mac in self.watched))
        return IN_GAME_REPORTED.get(reason) if watched else None


def boot_reason(event):
    """(reset reason, firmware) of a generator's device.boot record, or None."""
    fields = event.get("fields") or {}
    values = fields.get("values")
    if event.get("event_type") != "Boot" or fields.get("text") != "device.boot" or not isinstance(values, list) or len(values) < 2:
        return None
    reason, firmware = values[0], values[1]
    return (reason, firmware) if type(reason) is int and type(firmware) is int else None


SCAN_FAULT = "scan_fault"


def scan_fault_restart(event):
    """True for a generator's own restart after a Wi-Fi scan that never completed
    (wifi.offline.recovery stage 3, detail 4). Its Boot record is an ordinary SW restart."""
    fields = event.get("fields") or {}
    values = fields.get("values")
    return (event.get("event_type") == "Recovery" and fields.get("text") == "wifi.offline.recovery"
            and isinstance(values, list) and len(values) >= 4 and values[0] == 3 and values[3] == 4)


def replay(events, watch):
    """Decisions for a stored event stream (telemetry payload dicts in ingest order)."""
    decisions = []
    for event in events:
        fields = event.get("fields") or {}
        source, kind = event.get("source"), event.get("event_type")
        if source == "device":
            try:
                mac = normalize_mac(event.get("mac"))
            except ValueError:
                continue
            found = boot_reason(event) if mac in watch.names else None
            label = found and watch.reboot_label(found[0], mac)
            if label:
                decisions.append({"type": "reboot", "name": watch.names[mac], "reason": label,
                                  "firmware": found[1], "at": event.get("observed_utc"),
                                  "session": watch.session if mac in watch.watched else None})
            elif mac in watch.names and scan_fault_restart(event):
                decisions.append({"type": "reboot", "name": watch.names[mac], "reason": SCAN_FAULT,
                                  "firmware": None, "at": event.get("observed_utc"),
                                  "session": watch.session if mac in watch.watched else None})
            continue
        if source != "game_server" or not event.get("occurred_utc"):
            continue
        at = parse_utc(event["occurred_utc"])
        actions = watch.server(fields.get("boot_id"), at)
        if kind == "game_monitor_snapshot" and fields.get("snapshot_complete") is True:
            actions += watch.game(fields.get("game_active") is True, fields.get("game_session_id"), at)
        elif kind == "request_ingress":
            actions += watch.request(event.get("mac"), at)
        for action in actions:
            decisions.append(dict(action, session=watch.session, clock=watch.clock))
    return decisions


def main():
    parser = argparse.ArgumentParser(description="Replay stored telemetry through the generator watch (read-only)")
    parser.add_argument("--telemetry-database", required=True)
    parser.add_argument("--device", action="append", required=True, metavar="NAME=MAC")
    parser.add_argument("--always", action="append", default=[], metavar="NAME=MAC")
    parser.add_argument("--since", help="ISO UTC start of the replay")
    args = parser.parse_args()
    watch = GeneratorWatch(dict(item.split("=", 1) for item in args.device),
                           always=dict(item.split("=", 1) for item in args.always))
    uri = Path(args.telemetry_database).resolve().as_uri() + "?mode=ro"
    with closing(sqlite3.connect(uri, uri=True, timeout=5)) as raw:
        rows = raw.execute("SELECT e.payload FROM telemetry_ingest j JOIN telemetry_events e USING(site_id,source,source_id,event_id) "
                           "WHERE (e.source='device' OR json_extract(e.payload,'$.event_type') IN "
                           "('request_ingress','game_monitor_snapshot','server_start')) "
                           "AND (? IS NULL OR e.observed_utc>=?) ORDER BY j.seq", (args.since, args.since))
        for decision in replay((json.loads(row[0]) for row in rows), watch):
            print(json.dumps(decision, default=str, ensure_ascii=False))


if __name__ == "__main__":
    main()
