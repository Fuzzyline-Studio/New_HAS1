"""Store 3 watches one tag machine (UT) through the same rules store 1 uses for generators."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from generator_watch import GeneratorWatch  # noqa: E402
from incidents import IncidentStore, validate_generators  # noqa: E402
from protocol import ProtocolError, parse_batch  # noqa: E402
from telemetry import Event  # noqa: E402
from test_collector import TOPIC, payload  # noqa: E402
from test_generator_watch import stamp  # noqa: E402

UT = "2CBCBBA7C18C"
GLOVE = "A4F00FD90B34"
SITE, SERVER = "store-3", "store-3-game-server"


def request(mac, second, seq):
    return Event(site_id=SITE, source="game_server", source_id=SERVER, event_id=f"r{seq}",
                 observed_utc=stamp(second), occurred_utc=stamp(second), mac=mac, event_type="request_ingress",
                 fields={"boot_id": "server-1", "event_seq": seq, "request_type": "Loop"})


def snapshot(second, seq, active=True):
    return Event(site_id=SITE, source="game_server", source_id=SERVER, event_id=f"s{seq}",
                 observed_utc=stamp(second), occurred_utc=stamp(second), event_type="game_monitor_snapshot",
                 fields={"boot_id": "server-1", "event_seq": seq, "game_session_id": "game-1" if active else None,
                         "game_group": "g1" if active else None, "game_active": active, "snapshot_complete": True,
                         "theme": "error",
                         "participants": [{"device_id": "G1P1", "mac": GLOVE, "role": "player", "incident_id": None}] if active else [],
                         "active_incidents": []})


def boot(reason, second, device_boot=7):
    # What the tag machine firmware logs: Boot(esp_reset_reason, FIRMWARE_VER, PARTITION_VER, 0, "device.boot").
    return Event(site_id=SITE, source="device", source_id=UT, event_id=f"ring:{device_boot}",
                 observed_utc=stamp(second), mac=UT, device_id="HAS3_tagmachine", event_type="Boot",
                 fields={"ring": "ring", "seq": device_boot, "boot": device_boot, "uptime_ms": 65,
                         "values": [reason, 7, 1, 0], "text": "device.boot"})


class Store3TagMachineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = IncidentStore(Path(self.temp.name) / "incidents.db")
        self.store.start_run("run-1")
        config = validate_generators({"enabled": True, "devices": {"UT": "2C:BC:BB:A7:C1:8C"}, "role": "tagmachine"})
        self.store.attach_generators(GeneratorWatch(config["devices"], always=config["always"], role=config["role"]), SERVER)
        self.seq = 0

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def process(self, item, second):
        self.seq += 1
        self.store.process(self.seq, item, now=stamp(second), mono=100, run_id="run-1", channels=["slack"])
        self.store.db.execute("INSERT OR REPLACE INTO incident_meta VALUES ('delivery_run','run-1')")

    def traffic(self, start, end, *, ut=True, game=True):
        for second in range(start, end + 1):
            self.process(request(GLOVE, second, 1000 + self.seq), second)
            if ut:
                self.process(request(UT, second, 1000 + self.seq), second)
            if second % 5 == 0:
                self.process(snapshot(second, 1000 + self.seq, active=game), second)

    def incidents(self):
        return self.store.db.execute("SELECT device_id,state,reason,site_id FROM incident_state ORDER BY opened_utc").fetchall()

    def payloads(self):
        rows = []
        row = self.store.claim(run_id="run-1", mono=100, now_epoch=100)
        while row:
            rows.append(row["payload"])
            self.store.finish(row["id"], SimpleNamespace(state="delivered", retry_after=0, code="ok", remote_id="1.1"),
                              now_epoch=100, channels=["slack"])
            row = self.store.claim(run_id="run-1", mono=100, now_epoch=100)
        return rows

    def test_token_is_accepted_and_unknown_tokens_are_not(self):
        self.assertEqual(parse_batch(TOPIC, payload(device="HAS3_tagmachine")).device, "HAS3_tagmachine")
        with self.assertRaises(ProtocolError):
            parse_batch(TOPIC, payload(device="HAS3_duct"))

    def test_role_setting_is_validated(self):
        self.assertNotIn("role", validate_generators({"enabled": False}))
        for bad in ("Tag Machine", "", 3, "x" * 40):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                validate_generators({"enabled": True, "devices": {"UT": UT}, "role": bad})

    def test_silence_in_a_game_opens_and_recovers_by_mac(self):
        self.traffic(0, 10)
        self.traffic(11, 40, ut=False)
        self.assertEqual([tuple(row) for row in self.incidents()], [("UT", "open", "generator_silent", SITE)])
        opened = self.payloads()[0]  # announced while open; a silence that recovers first is never sent
        self.traffic(41, 45)
        self.assertEqual(self.incidents()[0]["state"], "recovered")
        self.assertEqual((opened["site_id"], opened["device_id"], opened["role"]), (SITE, "UT", "tagmachine"))
        self.assertTrue(opened["summary"].startswith("Tagmachine UT"))

    def test_silence_outside_a_game_is_not_reported(self):
        self.traffic(0, 10, game=False)
        self.traffic(11, 60, ut=False, game=False)
        self.assertEqual(self.incidents(), [])

    def test_crash_reboot_from_the_device_blackbox_is_reported(self):
        self.process(snapshot(0, 1, active=False), 0)
        self.process(boot(4, 10), 12)
        self.assertEqual([tuple(row)[:3] for row in self.incidents()], [("UT", "recovered", "generator_reboot")])
        sent = self.payloads()[0]
        self.assertEqual((sent["status"], sent["reset_label"], sent["role"]), ("rebooted", "panic", "tagmachine"))


if __name__ == "__main__":
    unittest.main()
