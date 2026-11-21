"""Fail-closed deploy settings, credential isolation, and old-target compatibility."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
from write_blackbox_release_config import PREFIXES, configuration, checked_broker, checked_password
from firmware_targets import TARGETS
from archive_iotglove_release import source_paths, DEVICES
from verify_blackbox_release import verify_firmware


class BlackboxReleaseTests(unittest.TestCase):
    def test_release_auto_enables_mqtt_only_for_blackbox_targets(self):
        for target in TARGETS:
            with self.subTest(target=target):
                if target in PREFIXES:
                    _, public = configuration(target, broker="mqtt://172.30.1.43:1883",
                                              password="deployment-password", release=True)
                    self.assertEqual(public["mode"], "mqtt")
                    for broker, password in ((None, "deployment-password"),
                                             ("mqtt://172.30.1.43:1883", None)):
                        with self.assertRaises(ValueError):
                            configuration(target, "auto", broker, password, release=True)
                    with self.assertRaises(ValueError):
                        configuration(target, "flash", release=True)
                else:
                    defines, public = configuration(target, release=True)
                    self.assertEqual(defines, "")
                    self.assertEqual(public["mode"], "not_applicable")

    def test_mqtt_requires_each_setting_and_supported_target(self):
        for target in PREFIXES:
            for broker, password in ((None, None), ("mqtt://172.30.1.43:1883", None),
                                     (None, "a-real-deployment-secret")):
                with self.subTest(target=target, broker=broker), self.assertRaises(ValueError):
                    configuration(target, "mqtt", broker, password)
        for target in set(TARGETS) - set(PREFIXES):
            with self.subTest(target=target), self.assertRaises(ValueError):
                configuration(target, "mqtt", "mqtt://172.30.1.43:1883", "deployment-password")

    def test_flash_does_not_embed_unused_credentials_for_any_target(self):
        for target in TARGETS:
            defines, public = configuration(target, "flash", "bad-uri", "should-never-appear")
            self.assertNotIn("should-never-appear", defines + json.dumps(public))
            self.assertEqual(public["broker_uri"], "")
            if target not in PREFIXES:
                self.assertEqual(defines, "")
                self.assertEqual(public["mode"], "not_applicable")

    def test_only_explicit_canonical_private_ipv4_brokers(self):
        for value in ("mqtt://172.30.1.43:1883", "mqtt://10.0.0.2:1", "mqtt://192.168.1.2:65535"):
            self.assertEqual(checked_broker(value), value)
        for value in ("", "mqtt://host:1883", "mqtt://127.0.0.1:1883", "mqtt://0.0.0.0:1883",
                      "mqtt://192.0.2.2:1883", "mqtt://239.1.1.1:1883", "mqtt://8.8.8.8:1883",
                      "mqtt://172.30.1.43:0", "mqtt://172.30.1.43:65536", "mqtt://172.30.1.43:01883",
                      "mqtt://172.030.1.43:1883", "mqtts://172.30.1.43:1883", "mqtt://172.30.1.43",
                      "mqtt://user:password@172.30.1.43:1883", "mqtt://172.30.1.43:1883/path",
                      "mqtt://172.30.1.43:1883?x=1", "mqtt://172.30.1.43:1883\n"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                checked_broker(value)

    def test_password_limits_and_placeholder_rejection(self):
        for value in ("", " ", "a" * 128, "x\n", "x\0", "x\x7f", "암호",
                      "REPLACE_WITH_PASSWORD", "test-only-password", "__COMPILE_ONLY_DO_NOT_DEPLOY__"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                checked_password(value)
        self.assertEqual(checked_password("a" * 127), "a" * 127)

    def run_helper(self, root, target, mode, broker="mqtt://172.30.1.43:1883", password='secure-quote"slash\\value', release=False):
        directory = root / TARGETS[target][0]
        directory.mkdir(parents=True, exist_ok=True)
        header = directory / "secrets.h"
        header.write_text("existing-header")
        public = root / "public.json"
        env = dict(os.environ, HMAC_SECRET="existing-hmac-signing-key", BLACKBOX_BROKER_URI=broker,
                   BLACKBOX_MQTT_PASSWORD=password)
        command = [sys.executable, str(ROOT / "scripts/write_blackbox_release_config.py"),
                   "--device", target, "--root", str(root), "--public-config", str(public)]
        if mode is not None:
            command += ["--mode", mode]
        if release:
            command += ["--release"]
        process = subprocess.run(command, env=env, capture_output=True, text=True)
        if password:
            self.assertNotIn(password, process.stdout + process.stderr)
        self.assertNotIn(env["HMAC_SECRET"], process.stdout + process.stderr)
        return process, header, public

    def test_cli_keeps_hmac_exact_and_public_metadata_credential_free(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for target, prefix in (("iotglove", "IOTGLOVE"), ("HAS1_generator", "HAS1_GENERATOR"),
                                   ("HAS1_altar", "HAS1_ALTAR")):
                process, header, public = self.run_helper(root, target, "mqtt")
                self.assertEqual(process.returncode, 0, process.stderr)
                macros = {line.split(" ", 2)[1]: json.loads(line.split(" ", 2)[2])
                          for line in header.read_text().splitlines() if line.startswith("#define ")}
                self.assertEqual(macros, {"HMAC_SECRET": "existing-hmac-signing-key",
                                         prefix + "_BLACKBOX_BROKER_URI": "mqtt://172.30.1.43:1883",
                                         prefix + "_BLACKBOX_MQTT_PASSWORD": 'secure-quote"slash\\value'})
                self.assertEqual(json.loads(public.read_text()), {"schema": 1, "device": target,
                                 "mode": "mqtt", "broker_uri": "mqtt://172.30.1.43:1883",
                                 "authentication": "device_mac"})

    def test_invalid_mqtt_leaves_header_untouched_and_no_public_file(self):
        with tempfile.TemporaryDirectory() as directory:
            process, header, public = self.run_helper(Path(directory), "iotglove", "mqtt", password="")
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(header.read_text(), "existing-header")
            self.assertFalse(public.exists())

    def test_release_cli_defaults_to_mqtt_and_rejects_flash_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process, _, public = self.run_helper(root, "iotglove", None, release=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(json.loads(public.read_text())["mode"], "mqtt")
            public.unlink()
            process, header, public = self.run_helper(root, "iotglove", "flash", release=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(header.read_text(), "existing-header")
            self.assertFalse(public.exists())

    def test_source_archive_hashes_helpers_but_never_secret_headers(self):
        for target in DEVICES:
            paths = source_paths(ROOT, target)
            self.assertIn("scripts/write_blackbox_release_config.py", paths)
            self.assertIn("scripts/write_firmware_secret.py", paths)
            self.assertIn("scripts/verify_blackbox_release.py", paths)
            self.assertFalse(any(Path(path).name == "secrets.h" for path in paths))

    def test_workflow_checks_config_before_bump_and_image_before_publish(self):
        workflow = (ROOT / ".github/workflows/deploy-firmware.yml").read_text()
        config = workflow.index("python3 scripts/write_blackbox_release_config.py")
        bump = workflow.index('ci_deploy.py" bump')
        placement = workflow.index('cp "$RUNNER_TEMP/build-out"/*.ino.bin')
        verify = workflow.index("python3 scripts/verify_blackbox_release.py")
        publish = workflow.index('ci_deploy.py" publish')
        self.assertLess(config, bump)
        self.assertLess(placement, verify)
        self.assertLess(verify, publish)
        self.assertIn('--mode "$BLACKBOX_MODE" --release', workflow[config:bump])
        self.assertIn("inputs.blackbox_mode || 'auto'", workflow)
        self.assertIn("default: auto", workflow)
        self.assertIn('--firmware "$DEVICE_DIR/update.bin"', workflow[verify:publish])
        compile_workflow = (ROOT / ".github/workflows/compile-iotglove.yml").read_text()
        self.assertEqual(compile_workflow.count('"scripts/verify_blackbox_release.py"'), 2)


class CompiledBlackboxReleaseTests(unittest.TestCase):
    broker = "mqtt://172.30.1.43:1883"
    password = "deployment-password"

    def mqtt_image(self):
        return (b"\xe9" + b"\0" * 23 + b"bb-writer\0bb-upload\0device.boot\0" +
                self.broker.encode() + b"\0" + self.password.encode() + b"\0")

    def config(self, target="iotglove"):
        return configuration(target, "auto", self.broker, self.password, release=True)[1]

    def test_mqtt_image_matches_config_for_both_targets(self):
        self.assertEqual(set(PREFIXES), {"iotglove", "HAS1_generator", "HAS1_altar"})
        for target in PREFIXES:
            self.assertTrue(verify_firmware(target, self.config(target), self.mqtt_image(), self.broker, self.password))

    def test_flash_metadata_or_stale_target_cannot_publish(self):
        flash = configuration("iotglove", "flash")[1]
        for public in (flash, self.config("HAS1_generator"), dict(self.config(), mode="flash"),
                       dict(self.config(), authentication="none"), dict(self.config(), broker_uri="")):
            with self.subTest(public=public), self.assertRaises(ValueError):
                verify_firmware("iotglove", public, self.mqtt_image(), self.broker, self.password)

    def test_image_must_contain_current_broker_credential_and_code(self):
        for marker in (self.broker.encode(), self.password.encode(), b"bb-writer", b"bb-upload", b"device.boot"):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                verify_firmware("iotglove", self.config(), self.mqtt_image().replace(marker, b"omitted"),
                                self.broker, self.password)
        for image in (b"", b"not-an-ESP32-image" + self.mqtt_image()):
            with self.assertRaises(ValueError):
                verify_firmware("iotglove", self.config(), image, self.broker, self.password)

    def test_other_targets_do_not_require_mqtt_settings_or_markers(self):
        for target in set(TARGETS) - set(PREFIXES):
            public = configuration(target, "auto", release=True)[1]
            self.assertFalse(verify_firmware(target, public, b"other-target-image"))

    def test_verifier_cli_rejects_missing_credential_without_disclosing_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            public = root / "public.json"
            image = root / "update.bin"
            public.write_text(json.dumps(self.config()))
            image.write_bytes(self.mqtt_image().replace(self.password.encode(), b"omitted"))
            env = dict(os.environ, BLACKBOX_BROKER_URI=self.broker, BLACKBOX_MQTT_PASSWORD=self.password)
            process = subprocess.run([sys.executable, str(ROOT / "scripts/verify_blackbox_release.py"),
                                      "--device", "iotglove", "--public-config", str(public),
                                      "--firmware", str(image)], env=env, capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertNotIn(self.password, process.stdout + process.stderr)
            self.assertNotIn(self.broker, process.stdout + process.stderr)


if __name__ == "__main__":
    unittest.main()
