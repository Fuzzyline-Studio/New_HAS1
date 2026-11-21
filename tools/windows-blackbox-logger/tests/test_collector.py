import io
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from collector import Collector, load_config
from export import export
from protocol import ProtocolError, parse_batch
from storage import IdentityCollision, Store
from helpers import MAC, RING, TOPIC, payload, record


class ProtocolTests(unittest.TestCase):
    def test_round_trip_extreme_signed_values_and_text(self):
        raw = record(0xFFFFFFFF, kind=15, values=(-2147483648, 2147483647, 0, -1), text=b"x" * 28)
        batch = parse_batch(TOPIC, payload(records=[raw]))
        self.assertEqual(batch.records[0].raw, raw)
        self.assertEqual(batch.records[0].text, "x" * 28)
        self.assertEqual(batch.records[0].values[0], -2147483648)
        self.assertEqual(batch.ack_payload, f"ACK1 {RING} 00000001 4294967295\n".encode())

    def test_gaps_allowed(self):
        self.assertEqual(len(parse_batch(TOPIC, payload((1, 3, 100))).records), 3)

    def test_both_device_types(self):
        self.assertEqual(parse_batch(TOPIC, payload(device="HAS1_generator")).device, "HAS1_generator")
        self.assertEqual(parse_batch(TOPIC, payload(device="HAS1_altar")).device, "HAS1_altar")
        with self.assertRaises(ProtocolError):
            parse_batch(TOPIC, payload(device="HAS1_duct"))

    def test_invalid_inputs(self):
        original = payload()
        changed = bytearray(record())
        changed[20] ^= 1
        cases = [b"", original[:-1], original + b"\n", original.replace(b"BB1", b"BB2"),
                 original.replace(b"iotglove", b"other"), original.replace(MAC.encode(), b"aabbccddeeff"),
                 original.replace(RING.encode(), RING.upper().encode()), b"x" * 4097,
                 original.replace(b"\n", b"\r\n"), payload(()), payload(tuple(range(17))),
                 payload((2, 1)), payload((1, 1)), payload(records=[bytes(changed)]),
                 payload(records=[record()[:-1]]), payload((0,)), payload(records=[record(kind=16)]),
                 original + b"\xff\n"]
        for bad in cases:
            with self.subTest(bad=bad[:60]), self.assertRaises(ProtocolError):
                parse_batch(TOPIC, bad)
        with self.assertRaises(ProtocolError):
            parse_batch(TOPIC.replace(MAC, "000000000000"), original)


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "logs.sqlite3"
        self.store = Store(self.path)
        self.sent = []
        self.collector = Collector(self.path, lambda topic, body: self.sent.append((topic, body)) or True)

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def count(self):
        return self.store.db.execute("SELECT count(*) FROM records").fetchone()[0]

    def test_commit_visible_to_new_connection_before_ack(self):
        def publish(topic, body):
            with closing(sqlite3.connect(self.path)) as reader:
                self.assertEqual(reader.execute("SELECT count(*) FROM records").fetchone()[0], 2)
            self.assertEqual(body, f"ACK1 {RING} 00000001 2\n".encode())
            return True
        self.collector.publish = publish
        self.assertTrue(self.collector.process(self.store, TOPIC, payload((1, 2))))
        self.assertEqual(self.store.db.execute("PRAGMA synchronous").fetchone()[0], 2)

    def test_duplicate_replay_and_lost_ack(self):
        self.collector.publish = lambda *_: False
        self.assertFalse(self.collector.process(self.store, TOPIC, payload((1, 2))))
        self.assertEqual(self.count(), 2)
        self.collector.publish = lambda t, b: self.sent.append((t, b)) or True
        self.assertTrue(self.collector.process(self.store, TOPIC, payload((1, 2))))
        self.assertEqual(self.count(), 2)
        self.assertEqual(len(self.sent), 1)

    def test_conflict_rolls_back_whole_batch_without_ack(self):
        self.store.save(parse_batch(TOPIC, payload((4,))))
        bad = payload(records=[record(3), record(4, text=b"collision")])
        with self.assertRaises(IdentityCollision):
            self.collector.process(self.store, TOPIC, bad)
        self.assertEqual(self.count(), 1)
        self.assertEqual(self.sent, [])

    def test_device_identity_conflict(self):
        self.store.save(parse_batch(TOPIC, payload()))
        with self.assertRaises(IdentityCollision):
            self.collector.process(self.store, TOPIC, payload(device="HAS1_generator"))
        self.assertEqual(self.sent, [])

    def test_readonly_database_does_not_ack(self):
        self.store.db.execute("PRAGMA query_only=ON")
        with self.assertRaises(sqlite3.OperationalError):
            self.collector.process(self.store, TOPIC, payload())
        self.assertEqual(self.sent, [])

    def test_real_sqlite_full_then_retry(self):
        pages = self.store.db.execute("PRAGMA page_count").fetchone()[0]
        self.store.db.execute(f"PRAGMA max_page_count={pages}")
        for start in range(1, 1000, 16):
            body = payload(tuple(range(start, start + 16)))
            before = len(self.sent)
            try:
                self.collector.process(self.store, TOPIC, body)
            except sqlite3.OperationalError as exc:
                self.assertIn("full", str(exc))
                self.assertEqual(len(self.sent), before)
                self.store.db.execute("PRAGMA max_page_count=10000")
                self.assertTrue(self.collector.process(self.store, TOPIC, body))
                break
        else:
            self.fail("SQLite capacity limit did not trigger")

    def test_export_and_filter(self):
        self.store.save(parse_batch(TOPIC, payload((1, 2))))
        output = io.StringIO()
        export(self.path, output, mac=MAC, limit=1)
        rows = [json.loads(row) for row in output.getvalue().splitlines()]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["raw_hex"], record().hex())
        with self.assertRaises(ValueError):
            export(self.path, io.StringIO(), limit=0)

    def test_future_database_version_rejected(self):
        self.store.db.execute("PRAGMA user_version=9")
        with self.assertRaises(sqlite3.DatabaseError):
            Store(self.path)


class CrashTests(unittest.TestCase):
    def test_subprocess_termination_commit_boundaries(self):
        for boundary, expected in (("before_commit", 0), ("after_commit", 2)):
            with self.subTest(boundary=boundary), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "crash.sqlite3"
                script = """
import os, sys
from protocol import parse_batch
from storage import Store
store = Store(sys.argv[1])
batch = parse_batch(sys.argv[2], bytes.fromhex(sys.argv[3]))
store.save(batch, lambda stage: os._exit(23) if stage == sys.argv[4] else None)
raise RuntimeError('crash hook did not run')
"""
                process = subprocess.run([sys.executable, "-c", script, str(path), TOPIC,
                                          payload((1, 2)).hex(), boundary], cwd=ROOT, timeout=10)
                self.assertEqual(process.returncode, 23)
                recovered = Store(path)
                try:
                    self.assertEqual(recovered.db.execute("SELECT count(*) FROM records").fetchone()[0], expected)
                    recovered.save(parse_batch(TOPIC, payload((1, 2))))
                    self.assertEqual(recovered.db.execute("SELECT count(*) FROM records").fetchone()[0], 2)
                finally:
                    recovered.close()

    def test_corrupt_database_is_not_recreated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "corrupt.sqlite3"
            content = b"corrupted database" * 100
            path.write_bytes(content)
            with self.assertRaises(sqlite3.DatabaseError):
                Store(path)
            self.assertEqual(path.read_bytes(), content)


class QueueTests(unittest.TestCase):
    def test_bounded_queue_retained_and_oversized_messages(self):
        collector = Collector("unused", lambda *_: True, queue_size=1)
        self.assertFalse(collector.submit(TOPIC, payload(), retained=True))
        self.assertFalse(collector.submit(TOPIC, b"x" * 4097))
        self.assertTrue(collector.submit(TOPIC, payload()))
        self.assertFalse(collector.submit(TOPIC, payload()))
        self.assertEqual(collector.counts["queue_full"], 1)

    def test_writer_survives_invalid_and_publish_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            sent = []
            def publish(topic, body):
                sent.append(body)
                if len(sent) == 1:
                    raise OSError("broker disconnected")
                return True
            collector = Collector(Path(directory) / "test.sqlite3", publish)
            collector.start()
            collector.submit(TOPIC, b"bad")
            collector.submit(TOPIC, payload())
            collector.submit(TOPIC, payload())
            collector.close()
            self.assertEqual(collector.counts["invalid_batches"], 1)
            self.assertEqual(collector.counts["committed_batches"], 2)
            self.assertEqual(collector.counts["acks_queued"], 1)
            self.assertFalse(collector.worker.is_alive())

    def test_invalid_database_start_fails_before_mqtt(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.sqlite3"
            path.write_bytes(b"broken" * 1024)
            collector = Collector(path, lambda *_: self.fail("must not publish"))
            with self.assertRaises(RuntimeError):
                collector.start()
            collector.close()

    def test_config_relative_path_and_invalid_port(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            config = json.loads((ROOT / "config.example.json").read_text())
            path.write_text(json.dumps(config))
            self.assertEqual(load_config(path)["database"], str((Path(directory) / "data/blackbox.sqlite3").resolve()))
            config["port"] = True
            path.write_text(json.dumps(config))
            with self.assertRaises(ValueError):
                load_config(path)


if __name__ == "__main__":
    unittest.main()
