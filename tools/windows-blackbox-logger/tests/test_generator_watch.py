"""Generator silence and crash-reboot rule: decisions, incident flow and cards."""
from dataclasses import replace
from datetime import timedelta
import json
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from contextlib import closing
from unittest.mock import patch

from generator_watch import GeneratorWatch, replay
from incident_evidence import summarize
from incidents import IncidentRuntime, IncidentStore, _open_telemetry, validate_generators
from notifications import _event_properties, _send_slack_session, slack_event_reply, slack_message, slack_session_card
from telemetry import Event, TelemetryStore, parse_utc

START = parse_utc("2026-10-24T23:00:00+00:00")
GENERATORS = {"BG": "A4F00FD90BCC", "LG": "A4F00FD906BC", "SG": "A4F00FD90A74", "TG": "B03FD32DFEE0", "UG": "A4F00FD90930"}
GLOVE = "A4F00FD90B34"
AG = "A4F00FD90B44"  # the academy generator: reboot alerts only
CARD, PAGE = "1700000000.000100", "abcdefab-1234-1234-1234-123456789abc"


def at(seconds):
    return START + timedelta(seconds=seconds)


def stamp(seconds):
    return at(seconds).isoformat(timespec="microseconds")


def traffic(watch, start, end, *, silent=(), step=1):
    """Every device requests each step seconds, except the named generators."""
    actions = []
    for second in range(start, end + 1, step):
        actions += watch.request(GLOVE, at(second))
        for name, mac in GENERATORS.items():
            if name not in silent:
                actions += watch.request(mac, at(second))
    return actions


class WatchTests(unittest.TestCase):
    def setUp(self):
        self.watch = GeneratorWatch(GENERATORS)
        self.watch.server("boot-1", at(0))

    def test_silence_needs_a_game_and_twenty_seconds(self):
        self.assertEqual(traffic(self.watch, 0, 40, silent=("BG",)), [])  # no game: never reported
        self.watch.game(True, "game-1", at(41))
        quiet = traffic(self.watch, 42, 60, silent=("BG",))
        self.assertEqual(quiet, [])  # counted from the game start, not from its last request at 0s
        opened = traffic(self.watch, 61, 61, silent=("BG",))
        self.assertEqual([(item["type"], item["names"], item["gap_ms"]) for item in opened], [("open", ["BG"], 20000)])
        self.assertEqual(traffic(self.watch, 62, 70, silent=("BG",)), [])  # reported once
        back = self.watch.request(GENERATORS["BG"], at(71))
        self.assertEqual([(item["type"], item["key"]) for item in back], [("recover", opened[0]["key"])])

    def test_server_wide_pause_is_not_a_generator_silence(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 10)
        # Nothing at all for 32s (server stall), then everything resumes.
        self.assertEqual(traffic(self.watch, 42, 60), [])
        # A generator that stays quiet afterwards is still caught, 20s after its last request (60s).
        self.assertEqual(traffic(self.watch, 61, 79, silent=("UG",)), [])
        self.assertEqual([item["names"] for item in traffic(self.watch, 80, 80, silent=("UG",))], [["UG"]])

    def test_generators_dropping_together_are_one_group(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 5)
        traffic(self.watch, 6, 7, silent=("BG", "LG"))
        actions = traffic(self.watch, 8, 30, silent=("BG", "LG", "SG", "TG", "UG"))
        self.assertEqual([(item["type"], item["names"]) for item in actions], [("open", ["BG", "LG", "SG", "TG", "UG"])])
        self.assertEqual(actions[0]["since"], at(5))
        # The group recovers once every member is back.
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(31)), [])
        partial = traffic(self.watch, 32, 32, silent=("UG",))
        self.assertEqual(partial, [])
        self.assertEqual([item["type"] for item in self.watch.request(GENERATORS["UG"], at(33))], ["recover"])

    def test_two_silent_generators_are_reported_separately(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 2)
        actions = traffic(self.watch, 3, 30, silent=("BG", "SG"))
        self.assertEqual(sorted(item["names"][0] for item in actions), ["BG", "SG"])

    def test_game_end_and_server_restart_end_open_silences(self):
        self.watch.game(True, "game-1", at(0))
        opened = traffic(self.watch, 0, 21, silent=("BG",))
        self.assertEqual(len(opened), 1)
        self.assertEqual(self.watch.game(False, None, at(22)), [{"type": "end", "key": opened[0]["key"], "reason": "game_ended"}])
        self.watch.game(True, "game-2", at(23))
        opened = traffic(self.watch, 23, 44, silent=("BG",))
        self.assertEqual(self.watch.server("boot-2", at(45)), [{"type": "end", "key": opened[0]["key"], "reason": "server_restarted"}])
        # The new server boot starts measurement again.
        self.assertEqual(traffic(self.watch, 46, 64, silent=("BG",)), [])

    def test_restored_incident_survives_its_game_and_recovers(self):
        self.watch.restore("incident-x", [GENERATORS["LG"]], at(0), "game-1")
        self.assertEqual(self.watch.game(True, "game-1", at(1)), [])
        self.assertEqual(self.watch.request(GENERATORS["LG"], at(2))[0], {"type": "recover", "key": "incident-x", "at": at(2)})
        self.watch.restore("incident-y", [GENERATORS["LG"]], at(3), "game-1")
        self.assertEqual(self.watch.game(True, "game-2", at(4)), [{"type": "end", "key": "incident-y", "reason": "game_ended"}])

    def test_member_that_came_back_is_watched_again(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 3)
        group = traffic(self.watch, 4, 24, silent=tuple(GENERATORS))
        self.assertEqual(len(group), 1)
        traffic(self.watch, 25, 26, silent=("UG",))  # four are back, UG is still out
        actions = traffic(self.watch, 27, 47, silent=("UG", "BG"))
        self.assertEqual([(item["type"], item["names"], item["last_seen"]) for item in actions], [("open", ["BG"], at(26))])

    def test_late_request_from_before_the_outage_is_not_a_recovery(self):
        self.watch.game(True, "game-1", at(0))
        traffic(self.watch, 0, 2)
        opened = traffic(self.watch, 3, 22, silent=("BG",))
        self.assertEqual([item["since"] for item in opened], [at(2)])
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(1)), [])
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(10)), [])  # stamped before it was reported
        self.assertEqual(self.watch.request(GENERATORS["BG"], at(23))[0]["type"], "recover")

    def test_short_silence_setting_does_not_group_healthy_generators(self):
        watch = GeneratorWatch(GENERATORS, silence_seconds=10)
        watch.server("boot-1", at(0))
        watch.game(True, "game-1", at(0))
        traffic(watch, 0, 2)
        self.assertEqual([item["names"] for item in traffic(watch, 3, 12, silent=("BG",))], [["BG"]])

    def test_late_line_from_previous_boot_is_not_a_restart(self):
        self.watch.game(True, "game-1", at(0))
        self.watch.server("boot-2", at(10))
        opened = traffic(self.watch, 10, 31, silent=("BG",))
        self.assertEqual(self.watch.server("boot-1", at(9)), [])
        self.assertEqual(self.watch.boot, "boot-2")
        self.assertEqual(len(self.watch.open), len(opened))

    def test_reboot_labels(self):
        self.assertEqual([self.watch.reboot_label(reason) for reason in (4, 5, 6, 7, 9, 14, 15, 1, 3)],
                         ["panic", "int_wdt", "task_wdt", "wdt", "brownout", "pwr_glitch", "cpu_lockup", None, None])
        self.watch.game(True, "game-1", at(0))
        self.assertEqual([self.watch.reboot_label(reason) for reason in (1, 2, 3)], ["poweron", "ext", None])
        # An always-watched device's power-on counts game or not; a game generator's only in a game.
        watch = GeneratorWatch(GENERATORS, always={"AG": AG})
        self.assertEqual([watch.reboot_label(1, AG), watch.reboot_label(1, GENERATORS["BG"]), watch.reboot_label(3, AG)],
                         ["poweron", None, None])

    def test_always_watched_device_is_reported_game_or_not(self):
        watch = GeneratorWatch(GENERATORS, always={"AG": AG})
        watch.server("boot-1", at(0))
        for second in range(0, 5):
            watch.request(GLOVE, at(second))
            watch.request(AG, at(second))
        # No game: the game generators are not watched, AG is, counted from its own last request.
        opened = [action for second in range(5, 30) for action in watch.request(GLOVE, at(second))]
        self.assertEqual([(item["names"], item["always"], item["since"], item["at"]) for item in opened], [(["AG"], True, at(4), at(24))])
        # Neither a game nor a server restart ends it: only its own return does.
        self.assertEqual(watch.game(True, "game-1", at(30)), [])
        self.assertEqual(watch.game(False, None, at(31)), [])
        self.assertEqual(watch.server("boot-2", at(32)), [])
        self.assertEqual([item["type"] for item in watch.request(AG, at(33))], ["recover"])
        # Off since before the collector started: counted from the first request it saw.
        fresh = GeneratorWatch(GENERATORS, always={"AG": AG})
        opened = [action for second in range(0, 21) for action in fresh.request(GLOVE, at(second))]
        self.assertEqual([(item["names"], item["since"], item["last_seen"]) for item in opened], [(["AG"], at(0), None)])

    def test_always_watched_device_is_never_grouped(self):
        watch = GeneratorWatch(GENERATORS, always={"AG": AG})
        watch.server("boot-1", at(0))
        watch.game(True, "game-1", at(0))
        for second in range(0, 3):
            for mac in [GLOVE, AG, *GENERATORS.values()]:
                watch.request(mac, at(second))
        opened = [action for second in range(3, 30) for action in watch.request(GLOVE, at(second))]
        self.assertEqual(sorted((tuple(item["names"]), item["always"]) for item in opened),
                         [(("AG",), True), (("BG", "LG", "SG", "TG", "UG"), False)])
        group = next(item["key"] for item in opened if not item["always"])
        self.assertEqual(watch.game(False, None, at(31)), [{"type": "end", "key": group, "reason": "game_ended"}])

    def test_restored_always_watched_outage_adopts_no_game(self):
        watch = GeneratorWatch(GENERATORS, always={"AG": AG})
        watch.restore("ag-1", [AG], at(0), "게임외-20261025", always=True)
        self.assertIsNone(watch.session)
        self.assertEqual(watch.game(True, "game-1", at(1)), [])
        self.assertEqual(watch.request(AG, at(2))[0], {"type": "recover", "key": "ag-1", "at": at(2)})

    def test_replay_reads_server_and_device_events(self):
        def server(kind, second, **fields):
            return {"source": "game_server", "event_type": kind, "occurred_utc": stamp(second),
                    "mac": fields.pop("mac", None), "fields": dict(fields, boot_id="boot-1")}
        events = [server("game_monitor_snapshot", 0, snapshot_complete=True, game_active=True, game_session_id="game-1")]
        for second in range(0, 25):
            events.append(server("request_ingress", second, mac=GLOVE))
            events += [server("request_ingress", second, mac=mac) for name, mac in GENERATORS.items() if name != "TG"]
        events.append({"source": "device", "event_type": "Boot", "mac": GENERATORS["SG"], "observed_utc": stamp(25),
                       "fields": {"text": "device.boot", "values": [4, 39, 1, 0]}})
        events.append({"source": "device", "event_type": "Boot", "mac": GENERATORS["SG"], "observed_utc": stamp(26),
                       "fields": {"text": "device.boot", "values": [3, 39, 1, 0]}})
        for detail in (0, 4):  # only detail 4 marks a restart after a scan that never completed
            events.append({"source": "device", "event_type": "Recovery", "mac": GENERATORS["LG"], "observed_utc": stamp(27),
                           "fields": {"text": "wifi.offline.recovery", "values": [3, 10000, 1, detail]}})
        decisions = replay(events, GeneratorWatch(GENERATORS))
        self.assertEqual([(item["type"], item.get("names") or item.get("name"), item.get("reason")) for item in decisions],
                         [("open", ["TG"], None), ("reboot", "SG", "panic"), ("reboot", "LG", "scan_fault")])

    def test_config(self):
        self.assertEqual(validate_generators({"enabled": False}),
                         {"enabled": False, "devices": {}, "always": {}, "silence_seconds": 20, "group_min": 3})
        self.assertEqual(validate_generators({"enabled": True, "devices": {"BG": "a4:f0:0f:d9:0b:cc"}})["devices"], {"BG": "a4:f0:0f:d9:0b:cc"})
        self.assertEqual(validate_generators({"enabled": True, "always": {"AG": AG}})["always"], {"AG": AG})
        for bad in ({"enabled": True}, {"enabled": True, "devices": {"BG": "nope"}},
                    {"enabled": True, "devices": {"BG": GLOVE, "LG": GLOVE}}, {"devices": {}, "silence_seconds": 5},
                    {"devices": {}, "extra": 1}, {"enabled": "yes", "devices": {}}, {"devices": {"B G": GLOVE}},
                    {"devices": {"BG": AG}, "always": {"AG": AG}}, {"devices": {"AG": GLOVE}, "always": {"AG": AG}},
                    {"always": ["AG"]}, {"devices": {}, "reboot_only": {"AG": AG}}):
            with self.assertRaises(ValueError):
                validate_generators(bad)


def request(mac, second, seq, boot="server-1"):
    return Event(site_id="store-1", source="game_server", source_id="server-1", event_id=f"r{seq}",
                 observed_utc=stamp(second), occurred_utc=stamp(second), mac=mac, event_type="request_ingress",
                 fields={"boot_id": boot, "event_seq": seq, "request_type": "Loop"})


def snapshot(second, seq, active=True, session="game-1"):
    return Event(site_id="store-1", source="game_server", source_id="server-1", event_id=f"s{seq}",
                 observed_utc=stamp(second), occurred_utc=stamp(second), event_type="game_monitor_snapshot",
                 fields={"boot_id": "server-1", "event_seq": seq, "game_session_id": session if active else None,
                         "game_group": "g2" if active else None, "game_active": active, "snapshot_complete": True, "theme": "origin",
                         "participants": [{"device_id": "G2P4", "mac": GLOVE, "role": "player", "incident_id": None}] if active else [],
                         "active_incidents": []})


def boot(name, reason, second, *, device_boot=7, mac=None):
    return Event(site_id="store-1", source="device", source_id=mac or GENERATORS[name], event_id=f"ring:{device_boot}",
                 observed_utc=stamp(second), mac=mac or GENERATORS[name], device_id="HAS1_generator", event_type="Boot",
                 fields={"ring": "ring", "seq": device_boot, "boot": device_boot, "uptime_ms": 65,
                         "values": [reason, 39, 1, 0], "text": "device.boot"})


def scan_fault(name, second, *, device_boot=7, stage=3, detail=4):
    return Event(site_id="store-1", source="device", source_id=GENERATORS[name], event_id=f"ring:recovery:{device_boot}:{stage}",
                 observed_utc=stamp(second), mac=GENERATORS[name], device_id="HAS1_generator", event_type="Recovery",
                 fields={"ring": "ring", "seq": 100 + device_boot, "boot": device_boot, "uptime_ms": 15000,
                         "values": [stage, 10000, 1, detail], "text": "wifi.offline.recovery"})


class IncidentFlowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "incidents.db"
        self.store = self.open_store()
        self.seq = 0

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def open_store(self, always=None):
        store = IncidentStore(self.path)
        store.start_run("run-1")
        store.attach_generators(GeneratorWatch(GENERATORS, always=always), "server-1")
        return store

    def watch_ag(self):
        self.store.close()
        self.store = self.open_store(always={"AG": AG})

    def process(self, item, second):
        self.seq += 1
        self.store.process(self.seq, item, now=stamp(second), mono=100, run_id="run-1", channels=["slack", "notion"])
        self.store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','run-1')")

    def traffic(self, start, end, *, silent=(), game=True):
        for second in range(start, end + 1):
            self.process(request(GLOVE, second, 1000 + self.seq), second)
            for name, mac in GENERATORS.items():
                if name not in silent:
                    self.process(request(mac, second, 1000 + self.seq), second)
            if second % 5 == 0:
                self.process(snapshot(second, 1000 + self.seq, active=game), second)

    def claim(self):
        return self.store.claim(run_id="run-1", mono=100, now_epoch=100)

    def deliver(self, row, remote_id=None):
        self.store.finish(row["id"], SimpleNamespace(state="delivered", retry_after=0, code="ok", remote_id=remote_id),
                          now_epoch=100, channels=["slack", "notion"])

    def announce(self):
        """Deliver everything queued, as the bot would: a card ts for Slack, a page id for Notion."""
        row = self.claim()
        while row:
            self.deliver(row, remote_id=CARD if row["destination"] == "slack" else PAGE)
            row = self.claim()

    def queued(self):
        rows = self.store.db.execute("SELECT version,destination,payload FROM incident_outbox WHERE state='pending' ORDER BY id")
        return [(version, destination, json.loads(payload)) for version, destination, payload in rows]

    def details(self, reason="reconnected"):
        return json.loads(self.store.db.execute("SELECT details FROM incident_state WHERE reason=?", (reason,)).fetchone()[0])

    def incidents(self):
        return [tuple(row) for row in self.store.db.execute("SELECT device_id,state,reason FROM incident_state ORDER BY opened_utc")]

    def outbox(self):
        return [tuple(row) for row in self.store.db.execute("SELECT version,destination,state FROM incident_outbox ORDER BY id")]

    def test_silence_alerts_once_survives_snapshots_and_recovers(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.assertEqual(self.incidents(), [("BG", "open", "generator_silent")])
        row = self.claim()
        payload = row["payload"]
        self.assertEqual((payload["status"], payload["device_id"], payload["role"], payload["kind"], payload["gap_ms"]),
                         ("offline", "BG", "generator", "silence", 20000))
        self.deliver(row)
        self.deliver(self.claim())
        # Snapshots list no server incident for it, yet it stays open.
        self.traffic(26, 30, silent=("BG",))
        self.assertEqual(self.incidents(), [("BG", "open", "generator_silent")])
        self.process(request(GENERATORS["BG"], 31, 9999), 31)
        self.assertEqual(self.incidents(), [("BG", "recovered", "reconnected")])
        self.assertEqual([self.claim()["payload"]["status"] for _ in range(1)], ["recovered"])

    def test_generators_dropping_together_are_one_incident_and_end_with_the_game(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 3)
        self.traffic(4, 25, silent=tuple(GENERATORS))
        self.assertEqual(self.incidents(), [("발전기 BG·LG·SG·TG·UG", "open", "generator_silent")])
        self.assertEqual(self.claim()["payload"]["members"], ["BG", "LG", "SG", "TG", "UG"])
        self.process(snapshot(26, 9000, active=False), 26)
        self.assertEqual(self.incidents(), [("발전기 BG·LG·SG·TG·UG", "closed", "game_ended")])

    def test_server_pause_and_no_game_raise_nothing(self):
        self.traffic(0, 40, silent=("BG",), game=False)
        self.traffic(41, 45)
        self.traffic(80, 95)  # every device silent 46-79s: a server stall
        self.assertEqual(self.incidents(), [])

    def test_restarted_collector_resumes_recovery(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("LG",))
        self.store.close()
        self.store = self.open_store()
        self.process(request(GENERATORS["LG"], 26, 9999), 26)
        self.assertEqual(self.incidents(), [("LG", "recovered", "reconnected")])

    def test_crash_reboot_is_announced_once_with_blackbox_table(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 4, 10), 12)
        self.process(boot("SG", 4, 10), 13)  # importer reconciliation re-reads the same record
        self.assertEqual(self.incidents(), [("SG", "recovered", "generator_reboot")])
        claimed = {}
        for _ in range(2):
            row = self.claim()
            claimed[row["destination"]] = row["payload"]
            self.deliver(row, remote_id="abcdefab-1234-1234-1234-123456789abc" if row["destination"] == "notion" else None)
        self.assertEqual(set(claimed), {"slack", "notion"})
        payload = claimed["notion"]
        self.assertEqual((payload["status"], payload["reset_label"], payload["game_session_id"]),
                         ("rebooted", "panic", "게임외-20261025"))
        # No follow-up for a reboot; the Notion page gets the blackbox table.
        self.assertEqual(self.outbox(), [(1, "slack", "delivered"), (1, "notion", "delivered"), (3, "notion", "pending")])

    def test_power_cycle_in_a_game_is_one_incident(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.announce()
        self.process(request(GENERATORS["BG"], 30, 9998), 30)
        self.process(boot("BG", 1, 35), 35)  # its Boot record arrives after it is back (12s in the field)
        self.assertEqual(self.incidents(), [("BG", "recovered", "reconnected")])
        self.assertEqual(self.details()["reboots"],
                         [{"name": "BG", "label": "poweron", "firmware": 39, "device_boot": 7, "observed_utc": stamp(35)}])
        queued = self.queued()
        self.assertEqual([(version, destination, payload.get("refresh")) for version, destination, payload in queued],
                         [(2, "slack", None), (2, "notion", None), (3, "notion", None), (4, "slack", True), (4, "notion", True)])
        self.assertNotIn("rebooted", [payload.get("status") for _, _, payload in queued])  # no reboot alert of its own
        # The recovery queued before the Boot record arrived now carries it too.
        self.assertIn("rebooted: BG (poweron)", queued[1][2]["summary"])
        self.assertEqual((queued[3][2]["slack_ts"], queued[3][2]["reboot"]["label"]), (CARD, "poweron"))
        self.assertEqual(queued[4][2]["notion_page_id"], PAGE)
        incident = self.store.db.execute("SELECT incident_id FROM incident_state").fetchone()[0]
        line = str(slack_session_card(queued[3][2], self.store.slack_session(incident)))
        self.assertIn("🟢 BG · 10/25 08:00:20 끊김 → 10초 만에 복구 · 전원 투입 재부팅", line)
        # A crash with no silence (back within 20s) is still its own reboot line.
        self.process(boot("SG", 4, 40, device_boot=9), 40)
        self.assertEqual(sorted(row[2] for row in self.incidents()), ["generator_reboot", "reconnected"])

    def test_refresh_is_rendered_when_sent(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.announce()
        self.process(boot("BG", 3, 26), 26)  # an unannounced software restart still explains the outage
        refresh = self.queued()[0][2]
        self.assertEqual((refresh["status"], refresh["reboot"]["label"]), ("offline", "sw"))
        self.process(request(GENERATORS["BG"], 30, 9998), 30)
        # Sent after the recovery (a retry, say): it must not show the outage open again.
        incident = self.store.db.execute("SELECT incident_id FROM incident_state").fetchone()[0]
        current = self.store.current(incident, refresh)
        self.assertEqual((current["status"], current["refresh"], current["slack_ts"], current["reboot"]), ("recovered", True, CARD, refresh["reboot"]))

    def test_reboot_merges_only_into_its_own_recent_outage(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 3)
        self.traffic(4, 25, silent=("BG", "LG", "SG"))
        self.assertEqual(self.incidents(), [("발전기 BG·LG·SG", "open", "generator_silent")])
        self.announce()
        self.process(boot("SG", 1, 26, device_boot=1), 26)
        self.process(boot("LG", 1, 27, device_boot=1), 27)
        self.process(boot("SG", 1, 28, device_boot=2), 28)  # a second reboot of the same member is news
        self.process(boot("UG", 4, 28, device_boot=1), 28)  # not a member
        self.process(boot("BG", 4, 29, device_boot=1), 29)  # a crash in a shared outage keeps its own table
        self.assertEqual([item["name"] for item in self.details("generator_silent")["reboots"]], ["SG", "LG"])
        self.assertEqual(sorted(row[0] for row in self.incidents() if row[2] == "generator_reboot"), ["BG", "SG", "UG"])
        incident = self.store.db.execute("SELECT incident_id FROM incident_state WHERE reason='generator_silent'").fetchone()[0]
        line = str(slack_session_card({}, self.store.slack_session(incident)))
        self.assertIn("장애중 · SG 전원 투입 재부팅 · LG 전원 투입 재부팅", line)  # a group line names the member

    def test_reboot_long_after_the_outage_is_its_own_event(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.announce()
        self.process(request(GENERATORS["BG"], 30, 9998), 30)
        self.process(boot("BG", 1, 331), 331)  # five minutes after it recovered
        self.assertEqual(sorted(row[2] for row in self.incidents()), ["generator_reboot", "reconnected"])
        self.assertNotIn("reboots", self.details())

    def test_reboot_is_not_hidden_in_an_outage_never_announced(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.process(request(GENERATORS["BG"], 26, 9999), 26)  # back before its alert went out: canceled
        self.process(boot("BG", 1, 30), 30)
        self.assertEqual(sorted(row[2] for row in self.incidents()), ["generator_reboot", "reconnected"])
        self.traffic(31, 50, silent=("LG",))
        self.store.db.execute("UPDATE incident_outbox SET state='permanent' WHERE incident_id IN "
                              "(SELECT incident_id FROM incident_state WHERE device_id='LG')")  # its alert failed for good
        self.process(boot("LG", 1, 51, device_boot=1), 51)
        self.assertEqual(sorted(row[0] for row in self.incidents() if row[2] == "generator_reboot"), ["BG", "LG"])

    def test_reboot_noted_before_its_outage_was_announced_is_sent_after_it(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.process(boot("BG", 1, 26), 26)  # its alert is still queued
        self.assertEqual([(version, destination) for version, destination, _ in self.queued()], [(1, "slack"), (1, "notion")])
        self.assertIn("rebooted: BG (poweron)", self.queued()[1][2]["summary"])
        row = self.claim()
        self.deliver(row, remote_id=CARD)
        refresh = self.queued()[-1]
        self.assertEqual((refresh[0], refresh[1], refresh[2]["reboot"]["label"], refresh[2]["slack_ts"]), (4, "slack", "poweron", CARD))

    def test_game_times_come_from_its_snapshots(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(snapshot(5, 2), 5)
        self.process(snapshot(10, 3), 10)
        times = self.store._session_time("store-1", "game-1")
        self.assertEqual((times["started_utc"], times["ended_utc"], times["game_group"], times["theme"]), (stamp(5), None, "g2", "origin"))
        self.process(snapshot(4, 4), 4)  # a late line of the same game moves its start back
        self.assertEqual(self.store._session_time("store-1", "game-1")["started_utc"], stamp(4))
        partial = snapshot(11, 5)
        partial.fields.update(game_active=False, snapshot_complete=False, game_group=None)
        self.process(partial, 11)  # an incomplete snapshot does not end the game
        self.assertIsNone(self.store._session_time("store-1", "game-1")["ended_utc"])
        self.traffic(11, 30, silent=("BG",))
        self.announce()
        # The game ends: recorded, and its card edited once more so it shows the end.
        self.process(snapshot(31, 9000, active=False), 31)
        self.assertEqual(self.store._session_time("store-1", "game-1")["ended_utc"], stamp(31))
        refresh = [payload for version, destination, payload in self.queued() if payload.get("refresh")]
        self.assertEqual([(item["slack_ts"], item["reboot"]) for item in refresh], [(CARD, None)])
        self.assertEqual(self.queued()[0][2]["session_started_utc"], stamp(4))  # for the Notion session title
        # A late line stamped before the next game began does not end it.
        self.process(snapshot(40, 9001, session="game-2"), 40)
        self.process(snapshot(35, 9002, active=False), 35)
        self.assertIsNone(self.store._session_time("store-1", "game-2")["ended_utc"])
        # Seen running after a recorded end (a wrong end): open again; a late line before it changes nothing.
        self.process(snapshot(41, 9003, active=False), 41)
        self.process(snapshot(39, 9004, session="game-2"), 39)
        self.assertEqual(self.store._session_time("store-1", "game-2")["ended_utc"], stamp(41))
        self.process(snapshot(42, 9005, session="game-2"), 42)
        self.assertIsNone(self.store._session_time("store-1", "game-2")["ended_utc"])
        self.assertEqual(self.store.slack_session(self.store.db.execute("SELECT incident_id FROM incident_state").fetchone()[0])["ended_utc"], stamp(31))

    def test_always_watched_power_cycle_outside_games_is_one_incident(self):
        self.watch_ag()
        self.process(snapshot(0, 1, active=False), 0)
        for second in range(0, 3):
            self.process(request(AG, second, 2000 + second), second)
        self.traffic(3, 25, game=False)  # AG switched off at 3s; no game running
        self.assertEqual(self.incidents(), [("AG", "open", "generator_silent")])
        row = self.store.db.execute("SELECT session_id,details FROM incident_state").fetchone()
        self.assertEqual((row[0], json.loads(row[1])["watch"]), ("게임외-20261025", "always"))
        # Sent although the server runs no game (or another one).
        row = self.claim()
        self.assertEqual((row["version"], row["payload"]["device_id"], row["payload"]["game_session_id"]), (1, "AG", "게임외-20261025"))
        self.assertIn("while always watched", row["payload"]["summary"])
        self.deliver(row, remote_id=CARD if row["destination"] == "slack" else PAGE)
        self.announce()
        self.process(snapshot(26, 5000), 26)  # a game starts and ends meanwhile
        self.process(snapshot(27, 5001, active=False), 27)
        self.assertEqual(self.incidents(), [("AG", "open", "generator_silent")])
        self.process(request(AG, 40, 2999), 40)  # switched back on
        self.process(boot("AG", 1, 52, mac=AG, device_boot=3), 52)
        self.assertEqual(self.incidents(), [("AG", "recovered", "reconnected")])
        self.assertEqual([item["label"] for item in self.details()["reboots"]], ["poweron"])

    def test_always_watched_outage_ends_once_nothing_watches_it(self):
        self.watch_ag()
        self.process(snapshot(0, 1, active=False), 0)
        self.process(request(AG, 0, 2000), 0)
        self.traffic(1, 25, game=False)
        self.store.close()
        self.store = self.open_store()  # AG taken out of the config while it is still off
        self.process(snapshot(26, 5000, active=False), 26)
        self.assertEqual(self.incidents(), [("AG", "closed", "watch_removed")])

    def test_restarted_collector_keeps_an_always_watched_outage(self):
        self.watch_ag()
        self.process(snapshot(0, 1, active=False), 0)
        self.process(request(AG, 0, 2000), 0)
        self.traffic(1, 25, game=False)
        self.watch_ag()  # the collector restarts while AG is still off
        self.assertIsNone(self.store.watch.session)  # the day card is no game to adopt
        self.process(snapshot(26, 5000), 26)
        self.process(snapshot(27, 5001, active=False), 27)
        self.assertEqual(self.incidents(), [("AG", "open", "generator_silent")])
        self.process(request(AG, 28, 2999), 28)
        self.assertEqual(self.incidents(), [("AG", "recovered", "reconnected")])

    def test_always_watched_crash_in_a_game_goes_to_the_day_card(self):
        self.watch_ag()
        self.process(snapshot(0, 1), 0)
        for second in range(0, 11):
            self.process(request(AG, second, 2000 + second), second)
            self.traffic(second, second)
        self.process(boot("AG", 4, 12, mac=AG, device_boot=2), 12)  # back within seconds: no outage
        self.assertEqual(self.incidents(), [("AG", "recovered", "generator_reboot")])
        # Filed with the day's out-of-game events, not on the card of a game it is not part of.
        self.assertEqual(self.store.db.execute("SELECT session_id FROM incident_state").fetchone()[0], "게임외-20261025")

    def test_scan_fault_restart_is_announced_from_its_recovery_record(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(scan_fault("SG", 10, stage=3, detail=0), 10)   # ordinary offline recovery restart
        self.process(scan_fault("SG", 10, stage=2, detail=4), 10)   # deferred with the worker-fault blocker
        self.process(boot("SG", 3, 11, device_boot=8), 11)          # the restart's own SW Boot record
        self.assertEqual(self.incidents(), [])
        self.process(scan_fault("SG", 12), 12)
        self.assertEqual(self.incidents(), [("SG", "recovered", "generator_reboot")])
        payload = self.claim()["payload"]
        self.assertEqual((payload["status"], payload["reset_label"]), ("rebooted", "scan_fault"))
        self.assertIn("Wi-Fi 스캔 정지 복구", json.dumps(slack_message(payload), ensure_ascii=False))

    def test_scan_fault_rtc_replay_keeps_original_boot_identity_after_quiet_window(self):
        self.process(snapshot(0, 1, active=False), 0)
        original = scan_fault("SG", 10, device_boot=7)
        self.process(original, 10)
        # Deduplication survives a collector restart, not just its in-memory watch.
        self.store.close()
        self.store = self.open_store()

        # RTC replay retains the original boot and uptime, but receives a new ring
        # sequence. Arrive after 600 seconds so the quiet window cannot hide a bug.
        replayed = replace(original, event_id="ring:recovery:207", observed_utc=stamp(611),
                           fields={**original.fields, "seq": 207})
        self.process(replayed, 611)
        restarted = boot("SG", 3, 612, device_boot=8)
        self.process(replace(restarted, fields={**restarted.fields, "seq": 208}), 612)
        self.assertEqual(self.incidents(), [("SG", "recovered", "generator_reboot")])

        # A different scan fault in the new boot is still news after the window;
        # deduplication must not permanently suppress this device's later faults.
        next_fault = scan_fault("SG", 613, device_boot=8)
        self.process(replace(next_fault, fields={**next_fault.fields, "seq": 209}), 613)
        rows = self.store.db.execute("SELECT details FROM incident_state ORDER BY opened_utc").fetchall()
        self.assertEqual([(json.loads(row[0])["device_boot"], json.loads(row[0])["reset_label"])
                          for row in rows], [(7, "scan_fault"), (8, "scan_fault")])

    def test_reboots_that_are_not_announced(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 1, 10, device_boot=1), 10)   # power-on outside a game
        self.process(boot("SG", 3, 10, device_boot=2), 10)   # OTA / self-recovery restart
        self.process(boot("SG", 4, 10, device_boot=3), 400)  # old backlog
        self.process(boot("SG", 4, 10, device_boot=4, mac=GLOVE), 10)  # not a generator
        self.assertEqual(self.incidents(), [])
        self.process(snapshot(20, 2), 20)
        self.process(boot("SG", 1, 21, device_boot=5), 21)   # power-on during a game
        self.assertEqual([row[0] for row in self.incidents()], ["SG"])
        self.assertEqual(self.store.db.execute("SELECT session_id FROM incident_state").fetchone()[0], "game-1")


    def test_pending_silence_alert_is_canceled_when_it_recovers_first(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 25, silent=("BG",))
        self.process(request(GENERATORS["BG"], 26, 9999), 26)
        self.assertEqual(self.outbox(), [(1, "slack", "canceled"), (1, "notion", "canceled")])
        self.assertIsNone(self.claim())

    def test_untracked_generator_silence_ends_with_the_game(self):
        self.process(snapshot(0, 1), 0)
        self.store.db.execute("INSERT INTO incident_state VALUES ('lost','store-1','server-1','server-1','game-1','BG',NULL,'open','generator_silent',?,?,'{}')",
                              (stamp(0), stamp(0)))
        self.store.close()
        self.store = self.open_store()  # details unreadable: the watch cannot re-adopt it
        self.process(snapshot(5, 2), 5)
        self.assertEqual(self.incidents()[0][1], "open")
        self.process(snapshot(10, 3, active=False), 10)
        self.assertEqual(self.incidents(), [("BG", "closed", "game_ended")])

    def test_glove_incidents_are_unchanged_with_the_watch(self):
        glove = Event(site_id="store-1", source="game_server", source_id="server-1", event_id="off", observed_utc=stamp(1),
                      occurred_utc=stamp(1), mac=GLOVE, device_id="G2P4", event_type="device_offline",
                      fields={"boot_id": "server-1", "game_session_id": "game-1", "incident_id": "glove-1", "reason": "timeout", "event_seq": 2})
        self.process(glove, 1)
        listed = snapshot(2, 3)
        listed.fields["participants"][0]["incident_id"] = "glove-1"
        listed.fields["active_incidents"] = [{"device_id": "G2P4", "mac": GLOVE, "incident_id": "glove-1", "opened_utc": stamp(1)}]
        self.process(listed, 2)
        self.assertEqual(self.claim()["payload"]["device_id"], "G2P4")
        self.process(snapshot(3, 4), 3)  # no longer listed by the server
        self.assertEqual(self.incidents(), [("G2P4", "closed", "snapshot_no_longer_active")])

    def test_rebuilt_watch_redecides_a_rolled_back_event(self):
        self.process(snapshot(0, 1), 0)
        self.traffic(0, 19, silent=("BG",))
        with patch.object(IncidentStore, "_queue", side_effect=RuntimeError("injected")):
            with self.assertRaises(RuntimeError):
                self.process(request(GLOVE, 20, 8888), 20)
        self.assertEqual(self.incidents(), [])
        self.assertEqual(self.store.watch.open, {})  # rolled back with the transaction
        self.assertEqual(self.store.watch.session, "game-1")  # nothing else forgotten
        self.process(request(GLOVE, 20, 8888), 20)  # the runtime retries the same event
        self.assertEqual(self.incidents(), [("BG", "open", "generator_silent")])

    def test_routing_and_runtime_wiring(self):
        config = {"database": str(self.path), "notifications": {"enabled": False},
                  "generators": {"enabled": True, "devices": GENERATORS, "silence_seconds": 20, "group_min": 3}}
        runtime = IncidentRuntime(config, telemetry_database=str(self.path), site_id="store-1", source_id="server-1")
        state = runtime._new_store()
        try:
            self.assertIsNotNone(state.watch)
            self.assertTrue(runtime._routed(boot("SG", 4, 0), state))
            self.assertTrue(runtime._routed(request(GLOVE, 0, 1), state))
            other = Event(site_id="store-2", source="device", source_id=GENERATORS["SG"], event_id="x", observed_utc=stamp(0), mac=GENERATORS["SG"])
            self.assertFalse(runtime._routed(other, state))
            state.watch = None
            self.assertFalse(runtime._routed(boot("SG", 4, 0), state))
        finally:
            state.close()

    def test_reboot_alert_retries_and_ages_out(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 4, 10, device_boot=1), 10)
        self.process(boot("SG", 4, 20, device_boot=2), 20)  # crash loop: one announcement per 10 minutes
        self.assertEqual(len(self.incidents()), 1)
        row = self.claim()
        self.store.finish(row["id"], SimpleNamespace(state="retryable", retry_after=0, code="http_503", remote_id=None),
                          now_epoch=100, channels=["slack", "notion"])
        self.assertEqual(self.outbox()[0][2], "retryable")  # a reboot is never canceled for being "closed"
        self.store.db.execute("UPDATE incident_outbox SET next_attempt=0")
        late = at(10).timestamp() + 3601
        self.assertIsNone(self.store.claim(run_id="run-1", mono=100, now_epoch=late))
        self.assertEqual([state for _, _, state in self.outbox()], ["canceled", "canceled"])  # both at once

    def test_reboot_limit_does_not_cross_into_a_game(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot("SG", 4, 10, device_boot=1), 10)
        self.process(snapshot(20, 2), 20)
        self.process(boot("SG", 1, 30, device_boot=2), 30)  # power-on during the game, 20s later
        self.assertEqual([row[0] for row in self.store.db.execute("SELECT session_id FROM incident_state ORDER BY opened_utc")],
                         ["게임외-20261025", "game-1"])

    def test_reboot_evidence_shows_the_boot_and_crash(self):
        telemetry = Path(self.temp.name) / "telemetry.db"
        store = TelemetryStore(telemetry)
        def record(seq, observed, kind, text, values, device_boot):
            return Event(site_id="store-1", source="device", source_id=GENERATORS["SG"], event_id=f"r1:{seq}",
                         observed_utc=observed, clock_quality="uptime_only", mac=GENERATORS["SG"], device_id="HAS1_generator",
                         event_type=kind, fields={"ring": "r1", "seq": seq, "boot": device_boot, "uptime_ms": 1000 * seq,
                                                  "values": list(values), "text": text})
        for seq in range(1, 6):
            store.save(record(seq, stamp(-60), "Health", "heap", (1, 2, 3, 4), 1))
        # One upload after the crash: Boot, the crash summary, then routine records, all received together.
        store.save(record(6, stamp(10), "Boot", "device.boot", (4, 39, 1, 0), 2))
        store.save(record(7, stamp(10), "Crash", "prior.core:mqtt_task", (0x4013017c, 28, 0, 2), 2))
        for seq in range(8, 16):
            store.save(record(seq, stamp(10), "Health", "heap", (1, 2, 3, 4), 2))
        store.close()
        self.process(snapshot(0, 1, active=False), 0)
        self.process(record(6, stamp(10), "Boot", "device.boot", (4, 39, 1, 0), 2), 10)
        incident = self.store.db.execute("SELECT * FROM incident_state").fetchone()
        with closing(_open_telemetry(telemetry)) as raw:
            evidence = summarize(raw, incident, stamp(400))
        text = str(evidence["rows"])
        self.assertIn("재부팅", text)
        self.assertIn("크래시", text)
        self.assertNotIn("기록 없음", " ".join(evidence["highlights"]))


class CardTests(unittest.TestCase):
    REBOOT = {"incident_id": "incident-r", "site_id": "store-1", "device_id": "SG", "device_mac": "A4F00FD90A74",
              "game_session_id": "게임외-20261025", "status": "rebooted", "role": "generator", "kind": "reboot",
              "reset_label": "panic", "firmware": 39, "occurred_at": stamp(0), "opened_at": stamp(0),
              "close_reason": "generator_reboot"}

    def test_reboot_cards(self):
        text = str(slack_message(self.REBOOT))
        self.assertIn("🔁 장치 재부팅", text)
        self.assertIn("패닉(크래시)", text)
        self.assertIn("재부팅", slack_event_reply(self.REBOOT, {})["text"])
        card = slack_session_card(self.REBOOT, {"incidents": [{"device_id": "SG", "state": "recovered", "reason": "generator_reboot",
                                                              "opened_utc": stamp(0), "updated_utc": stamp(0)}]})
        self.assertIn("🔁 SG", str(card))
        properties = _event_properties(self.REBOOT, "rebooted", "incident-r")
        self.assertEqual(properties["이벤트"]["title"][0]["text"]["content"], "SG 재부팅")
        self.assertEqual(properties["상태"]["status"]["name"], "복구됨")
        self.assertEqual(properties["종료 사유"]["select"]["name"], "재부팅 패닉(크래시)")
        self.assertNotIn("지속(초)", properties)

    def test_reboot_reply_stays_in_the_thread(self):
        calls = []
        def call(url, method, body, token):
            calls.append((url.rsplit("/", 1)[-1], body.get("thread_ts"), body.get("reply_broadcast")))
            return {"status": 200, "body": '{"ok": true, "ts": "1700000000.000100"}'}
        result = _send_slack_session(call, "xoxb-synthetic-token", "C0123456789", self.REBOOT)
        self.assertEqual(result.state, "delivered")
        self.assertEqual(calls, [("chat.postMessage", None, None), ("chat.postMessage", "1700000000.000100", None)])
        # So does a device unrecovered at game end: no reply is shown in the channel.
        calls.clear()
        _send_slack_session(call, "xoxb-synthetic-token", "C0123456789", dict(self.REBOOT, status="monitoring_ended"))
        self.assertEqual(calls[-1][1:], ("1700000000.000100", None))

    def test_a_reboot_noted_on_its_silence_shows_in_that_line(self):
        reboots = json.dumps([{"name": "BG", "label": "poweron"}])  # as the collector's json_extract returns it
        line = slack_session_card(self.REBOOT, {"incidents": [{"device_id": "BG", "state": "recovered", "reason": "reconnected",
                                                              "reboots": reboots, "opened_utc": stamp(0), "updated_utc": stamp(30)}]})
        self.assertIn("BG · 10/25 08:00:00 끊김 → 30초 만에 복구 · 전원 투입 재부팅", str(line))
        broken = slack_session_card(self.REBOOT, {"incidents": [{"device_id": "BG", "state": "open", "reboots": "{bad"}]})
        self.assertIn("장애중", str(broken))

    def test_noted_reboot_edits_the_card_and_stays_in_the_thread(self):
        calls = []
        def call(url, method, body, token):
            calls.append((url.rsplit("/", 1)[-1], body.get("thread_ts"), body.get("reply_broadcast"), body.get("text")))
            return {"status": 200, "body": json.dumps({"ok": True, "ts": CARD})}
        silence = dict(self.REBOOT, device_id="BG", status="recovered", kind="silence", slack_ts=CARD, refresh=True)
        noted = dict(silence, reboot={"name": "BG", "label": "poweron", "firmware": 39, "observed_utc": stamp(35)})
        self.assertEqual(_send_slack_session(call, "xoxb-synthetic-token", "C0123456789", noted).state, "delivered")
        self.assertEqual([item[:3] for item in calls], [("chat.update", None, None), ("chat.postMessage", CARD, None)])
        self.assertIn("BG 재부팅 · 전원 투입 · 펌웨어 39", calls[1][3])
        calls.clear()
        _send_slack_session(call, "xoxb-synthetic-token", "C0123456789", silence)  # nothing to note: the edit is all
        self.assertEqual([item[0] for item in calls], ["chat.update"])

    def test_game_end_close_reason_is_readable(self):
        payload = dict(self.REBOOT, status="monitoring_ended", close_reason="game_ended", kind="silence")
        self.assertIn("게임 종료", slack_event_reply(payload, {})["text"])


if __name__ == "__main__":
    unittest.main()
