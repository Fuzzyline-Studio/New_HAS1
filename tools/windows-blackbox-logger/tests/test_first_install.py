import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from first_install import inventory, prepare, read_provision, broker_xml
from test_mqtt import BROKER, PASSWD, HAS_PAHO

MAC = "AABBCCDDEEFF"
OTHER = "112233445566"
TRAINING = "A4F00FD90B44"
ELSEWHERE = "2CBCBBA7BD28"
PROVISION = dict(schema_version=1, site_id="store-1", broker_uri="mqtt://172.30.1.43:1883",
                 collector_username="collector", device_password="d" * 64, collector_password="c" * 64)


class FirstInstallTests(unittest.TestCase):
    def test_inventory_read_only_and_explicit_allowlist(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.sqlite"
            db = sqlite3.connect(path)
            db.executescript("CREATE TABLE device(mac TEXT, device_type TEXT, theme TEXT); CREATE TABLE training_device(mac TEXT, device_type TEXT);")
            db.executemany("INSERT INTO device VALUES (?,?,?)", [("aa:bb:cc:dd:ee:ff", "originglove", "origin"),
                (OTHER, "generator", "origin"), ("", "originglove", "origin"), ("mac not found", "iotglove", "origin"),
                ("CC" * 6, "itembox", "origin"), (ELSEWHERE, "iotglove", "badland"), (ELSEWHERE, "iotglove", "origin_error")])
            db.executemany("INSERT INTO training_device VALUES (?,?)", [(TRAINING, "generator"), ("", "iotglove")])
            db.commit(); db.close()
            before = path.read_bytes()
            self.assertEqual(inventory(database=path), ([OTHER, TRAINING, MAC], 3))
            self.assertEqual(inventory(database=path, tables=["device"]), ([OTHER, MAC], 2))
            self.assertEqual(inventory(database=path, tables=["device"], theme="badland"), ([ELSEWHERE], 0))
            self.assertEqual(path.read_bytes(), before)
            with self.assertRaises(ValueError): inventory(database=path, tables=["device;DROP TABLE device"])
            for theme in ("", "origin' OR '1'='1", None):
                with self.assertRaises(ValueError): inventory(database=path, theme=theme)
            unthemed = Path(directory) / "unthemed.sqlite"
            db = sqlite3.connect(unthemed)
            db.executescript("CREATE TABLE device(mac TEXT, device_type TEXT);")
            db.execute("INSERT INTO device VALUES (?,?)", (MAC, "iotglove"))
            db.commit(); db.close()
            with self.assertRaises(ValueError): inventory(database=unthemed, tables=["device"])
            missing = Path(directory) / "absent.sqlite"
            with self.assertRaises(FileNotFoundError): inventory(database=missing)
            self.assertFalse(missing.exists())
            listing = Path(directory) / "macs.txt"
            listing.write_text("# STA MACs\naa:bb:cc:dd:ee:ff\nAABBCCDDEEFF\n")
            self.assertEqual(inventory(mac_list=listing), ([MAC], 0))
            listing.write_text("not a MAC")
            with self.assertRaises(ValueError): inventory(mac_list=listing)

    def test_provision_rejects_wrong_site_or_injection_without_echo(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "private.json"
            path.write_text(json.dumps(PROVISION))
            self.assertEqual(read_provision(path), PROVISION)
            path.write_text(json.dumps(dict(PROVISION, device_password="d" * 127, collector_password="c" * 128)))
            self.assertEqual(len(read_provision(path)["device_password"]), 127)
            path.write_text(json.dumps(dict(PROVISION, device_password="d" * 128)))
            with self.assertRaises(ValueError): read_provision(path)
            for key, bad in (("broker_uri", "mqtt://elsewhere:1883"), ("device_password", "pw\nadmin:pw"),
                             ("collector_username", "admin"), ("site_id", "store-2")):
                path.write_text(json.dumps(dict(PROVISION, **{key: bad})))
                with self.assertRaises(ValueError): read_provision(path)

    def test_hash_failure_cleans_plaintext_and_uses_no_password_argv(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch("first_install.subprocess.run", side_effect=subprocess.TimeoutExpired("tool", 30)) as run:
                with self.assertRaises(subprocess.TimeoutExpired): prepare(directory, PROVISION, [MAC], "passwd")
                argv = str(run.call_args)
                self.assertNotIn(PROVISION["device_password"], argv)
                self.assertNotIn(PROVISION["collector_password"], argv)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_broker_wrapper_has_no_secrets_and_localservice(self):
        xml = broker_xml("C:/Program Files/mosquitto/mosquitto.exe", "C:/ProgramData/HAS1BlackBoxBroker")
        self.assertIn("LocalService", xml)
        self.assertIn("roll-by-size", xml)
        self.assertNotIn(PROVISION["device_password"], xml)
        document = ET.fromstring(xml)
        self.assertEqual(document.findtext("hidewindow"), "false")
        self.assertEqual(document.findtext("stopparentprocessfirst"), "true")
        self.assertEqual(document.findtext("stoptimeout"), "30 sec")
        self.assertIsNone(document.find("stoparguments"))


@unittest.skipUnless(BROKER and PASSWD and HAS_PAHO, "Requires real Mosquitto and Paho")
class ProvisionedBrokerTests(unittest.TestCase):
    def test_generated_credentials_authentication_and_topic_isolation(self):
        import paho.mqtt.client as mqtt
        with tempfile.TemporaryDirectory(dir=os.environ.get("MOSQUITTO_TEST_TMPDIR")) as directory:
            base = Path(directory) / "Program Data with spaces"
            base.mkdir()
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
            prepare(base, PROVISION, [MAC, OTHER], PASSWD, host="127.0.0.1", port=port)
            hashes = (base / "passwords").read_text()
            self.assertNotIn(PROVISION["device_password"], hashes)
            self.assertFalse((base / ".passwords-plaintext.tmp").exists())
            config = json.loads((base / "collector-config.json").read_text())
            self.assertTrue(config["telemetry"]["clock_probe"]["enabled"])
            self.assertTrue(config["telemetry"]["device_import"]["enabled"])
            self.assertFalse(config["telemetry"]["omada"]["enabled"])
            with self.assertRaises(ValueError): prepare(base, PROVISION, [MAC], PASSWD)
            process = subprocess.Popen([BROKER, "-c", str(base / "mosquitto.conf")],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            clients = []
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=.1): break
                    except OSError:
                        if time.monotonic() > deadline: self.fail("Generated broker config failed to listen")
                        time.sleep(.05)
                def connect(username, password, subscription=None, accepted=True):
                    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
                    clients.append(client)
                    done, received, reasons = threading.Event(), [], []
                    client.username_pw_set(username, password)
                    def connected(c, u, f, rc, props):
                        reasons.append(rc)
                        if subscription and not rc.is_failure: c.subscribe(subscription, qos=1)
                        else: done.set()
                    client.on_connect = connected
                    client.on_subscribe = lambda *a: done.set()
                    client.on_message = lambda c, u, m: received.append((m.topic, m.payload))
                    client.connect("127.0.0.1", port, 5); client.loop_start()
                    self.assertTrue(done.wait(5))
                    self.assertEqual(not reasons[0].is_failure, accepted)
                    return client, received
                collector, batches = connect("collector", PROVISION["collector_password"], "has1/blackbox/v1/+/batch")
                device, acks = connect(MAC, PROVISION["device_password"], f"has1/blackbox/v1/{MAC}/ack")
                attacker, _ = connect(OTHER, PROVISION["device_password"])
                connect(MAC, "incorrect", accepted=False)
                connect("FFEEDDCCBBAA", PROVISION["device_password"], accepted=False)
                topic = f"has1/blackbox/v1/{MAC}/batch"
                device.publish(topic, b"valid", qos=1).wait_for_publish(3)
                attacker.publish(topic, b"forged", qos=1).wait_for_publish(3)
                device.publish(f"has1/blackbox/v1/{MAC}/ack", b"forged-ack", qos=1).wait_for_publish(3)
                collector.publish(f"has1/blackbox/v1/{MAC}/ack", b"valid-ack", qos=1).wait_for_publish(3)
                deadline = time.monotonic() + 3
                while (not batches or not acks) and time.monotonic() < deadline: time.sleep(.05)
                time.sleep(.3)
                self.assertEqual(batches, [(topic, b"valid")])
                self.assertEqual(acks, [(f"has1/blackbox/v1/{MAC}/ack", b"valid-ack")])
            finally:
                for client in clients: client.disconnect(); client.loop_stop()
                process.terminate(); process.wait(timeout=5)


if __name__ == "__main__": unittest.main()
