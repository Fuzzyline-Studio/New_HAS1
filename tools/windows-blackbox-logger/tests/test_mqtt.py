"""Real broker/collector integration; optional locally, required in Linux CI."""
import importlib.util
from contextlib import closing
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from helpers import MAC, RING, TOPIC, payload

BROKER = os.environ.get("MOSQUITTO_BIN") or shutil.which("mosquitto")
PASSWD = os.environ.get("MOSQUITTO_PASSWD_BIN") or shutil.which("mosquitto_passwd")
if not PASSWD and BROKER:
    # Homebrew keeps the broker in sbin and passwd in bin; Windows installers
    # put both executables together. A standalone collector need not have either.
    broker_path = Path(BROKER)
    for candidate in (broker_path.with_name("mosquitto_passwd"),
                      broker_path.with_name("mosquitto_passwd.exe"),
                      broker_path.parent.parent / "bin" / "mosquitto_passwd"):
        if candidate.is_file():
            PASSWD = str(candidate)
            break
try:
    HAS_PAHO = importlib.util.find_spec("paho.mqtt.client") is not None
except ModuleNotFoundError:
    HAS_PAHO = False


@unittest.skipUnless(BROKER and PASSWD and HAS_PAHO,
                     "install Mosquitto, mosquitto_passwd and requirements.txt for authenticated MQTT test")
class MqttIntegrationTests(unittest.TestCase):
    def test_persist_ack_duplicate_and_broker_restart(self):
        import paho.mqtt.client as mqtt
        with tempfile.TemporaryDirectory(dir=os.environ.get("MOSQUITTO_TEST_TMPDIR")) as directory:
            base = Path(directory)
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            broker_config = base / "mosquitto.conf"
            passwords = base / "passwords"
            attacker_mac = "112233445566"
            for index, username in enumerate(("collector", MAC, attacker_mac)):
                command = [PASSWD, "-b"] + (["-c"] if index == 0 else [])
                subprocess.run(command + [str(passwords), username, "integration-test"],
                               check=True, capture_output=True)
            acl = base / "acl"
            shutil.copyfile(ROOT / "mosquitto" / "acl.example", acl)
            broker_config.write_text(
                f"listener {port} 127.0.0.1\nallow_anonymous false\n"
                f"password_file {passwords.as_posix()}\nacl_file {acl.as_posix()}\n"
                "persistence false\nmessage_size_limit 4096\nlog_type all\n",
                encoding="utf-8")
            config = json.loads((ROOT / "config.example.json").read_text())
            config.update(port=port, database="test.sqlite3")
            config["telemetry"]["clock_probe"] = {"enabled": True}
            config["telemetry"]["device_import"] = {"enabled": True}
            config_path = base / "config.json"
            config_path.write_text(json.dumps(config))
            broker = None
            collector = None
            device = None
            attacker = None
            broker_output = (base / "broker.log").open("wb")
            collector_output = (base / "collector-process.log").open("wb")
            def start_broker():
                process = subprocess.Popen([BROKER, "-c", str(broker_config)],
                                           stdout=broker_output, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        self.fail("Mosquitto exited: " + (base / "broker.log").read_text())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            return process
                    except OSError:
                        time.sleep(0.05)
                process.terminate()
                process.wait(timeout=5)
                self.fail("Mosquitto did not listen")
            try:
                broker = start_broker()
                env = dict(os.environ, HAS1_BLACKBOX_PASSWORD="integration-test")
                collector = subprocess.Popen([sys.executable, str(ROOT / "collector.py"), "--config", str(config_path)],
                                             env=env, stdout=collector_output, stderr=subprocess.STDOUT)
                connected = threading.Event()
                acked = threading.Event()
                received = []
                device = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                     client_id="blackbox-integration-device", protocol=mqtt.MQTTv311)
                device.username_pw_set(MAC, "integration-test")
                device.reconnect_delay_set(min_delay=1, max_delay=2)
                device.on_connect = lambda c, u, f, reason, p: c.subscribe(f"has1/blackbox/v1/{MAC}/ack", qos=1)
                device.on_subscribe = lambda *args: connected.set()
                def on_ack(client, userdata, message):
                    if message.payload.startswith(b"T1 "):
                        token=message.payload.decode().split()[1]
                        response={"v":1,"kind":"time_probe","mac":MAC,"ring":RING,"boot":7,
                                  "token":token,"uptime_ms":12345,"seq_min":1,"seq_max":2,"record_uptime_wraps":0}
                        client.publish(TOPIC,json.dumps(response).encode(),qos=1)
                        return
                    received.append(message.payload)
                    acked.set()
                device.on_message = on_ack
                device.connect("127.0.0.1", port, keepalive=5)
                device.loop_start()
                self.assertTrue(connected.wait(5))
                def send_until_ack():
                    acked.clear()
                    deadline = time.monotonic() + 20
                    while time.monotonic() < deadline:
                        if collector.poll() is not None:
                            self.fail("Collector exited: " + (base / "collector-process.log").read_text())
                        device.publish(TOPIC, payload((1, 2)), qos=1)
                        if acked.wait(0.3):
                            break
                    self.assertTrue(acked.is_set(), (base / "collector-process.log").read_text())
                    self.assertEqual(received[-1], f"ACK1 {RING} 00000001 2\n".encode())
                    with closing(sqlite3.connect(base / "test.sqlite3")) as reader:
                        self.assertEqual(reader.execute("SELECT count(*) FROM records").fetchone()[0], 2)
                send_until_ack()
                send_until_ack()  # Application ACK loss: same batch, still two records.
                deadline=time.monotonic()+5
                anchored=False
                while time.monotonic()<deadline:
                    try:
                        with closing(sqlite3.connect(base / "data" / "telemetry.sqlite3")) as reader:
                            row=reader.execute("SELECT payload FROM clock_anchors").fetchone()
                            imported=reader.execute("SELECT count(*) FROM telemetry_events WHERE source='device'").fetchone()[0]
                        if row and imported==2:
                            self.assertEqual(json.loads(row[0])["clock_quality"],"pc_unverified")
                            anchored=True
                            break
                    except sqlite3.Error:
                        pass
                    time.sleep(.05)
                self.assertTrue(anchored,"real broker probe and independent device import must complete")

                # Exercise the shipped ACL, not a synthetic allow-all test rule.
                # MQTT 5 supplies an explicit denial reason in PUBACK; the
                # production device remains MQTT 3.1.1 as configured above.
                attacker_ready, rejected = threading.Event(), threading.Event()
                rejection_reasons = []
                attacker = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                       client_id="blackbox-acl-test", protocol=mqtt.MQTTv5)
                attacker.username_pw_set(attacker_mac, "integration-test")
                def attacker_connect(client, userdata, flags, reason, properties):
                    if not reason.is_failure:
                        attacker_ready.set()
                def attacker_publish(client, userdata, mid, reason, properties):
                    rejection_reasons.append(reason)
                    rejected.set()
                attacker.on_connect = attacker_connect
                attacker.on_publish = attacker_publish
                attacker.connect("127.0.0.1", port, keepalive=5)
                attacker.loop_start()
                self.assertTrue(attacker_ready.wait(5), "other valid device must authenticate")
                forged = b"FORGED-ACK-MUST-NOT-ARRIVE"
                for forbidden_topic, forbidden_payload in (
                    (f"has1/blackbox/v1/{MAC}/ack", forged),
                    (TOPIC, payload((1, 2, 3))),
                ):
                    rejected.clear()
                    attacker.publish(forbidden_topic, forbidden_payload, qos=1)
                    self.assertTrue(rejected.wait(5), "broker must acknowledge denied publish")
                    self.assertEqual(rejection_reasons[-1].value, 0x87, "Not authorized required")
                time.sleep(0.2)  # Drain delivery callbacks before checking no forged ACK.
                self.assertNotIn(forged, received)
                with closing(sqlite3.connect(base / "test.sqlite3")) as reader:
                    self.assertEqual(reader.execute("SELECT count(*) FROM records").fetchone()[0], 2)
                attacker.disconnect()
                attacker.loop_stop()
                attacker = None
                broker.terminate()
                broker.wait(timeout=5)
                connected.clear()
                broker = start_broker()
                self.assertTrue(connected.wait(10))
                send_until_ack()  # Both client subscriptions/reconnect and DB replay survive.
            finally:
                if attacker is not None:
                    attacker.disconnect()
                    attacker.loop_stop()
                if device is not None:
                    device.disconnect()
                    device.loop_stop()
                for process in (collector, broker):
                    if process is not None and process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)
                broker_output.close()
                collector_output.close()


if __name__ == "__main__":
    unittest.main()
