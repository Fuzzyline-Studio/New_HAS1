#!/usr/bin/env python3
"""Exercise production chip/crank/pulse and boot functions with fake hardware."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(file, name):
    source = (ROOT / file).read_text()
    start = source.index("void " + name + "(")
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


constants = (ROOT / "library_and_pin.h").read_text()
defines = "\n".join(re.findall(
    r"^#define (?:SOLENOID_PIN|SOLENOID_PULSE_MS|IR_SENSOR_PIN|"
    r"IR_SENSOR_DEBOUNCE_MS|MICRO_SW_PIN|MICRO_SW_DEBOUNCE_MS)\b.*$",
    constants, re.MULTILINE))
defines += "\n" + re.search(
    r"^#define FIRMWARE_VER\b.*$", (ROOT / "HAS1_altar.ino").read_text(),
    re.MULTILINE).group()

prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using String = std::string;
constexpr int LOW = 0, HIGH = 1, OUTPUT = 2;
struct Value {
    std::string text;
    Value& operator=(const char* value) { text = value; return *this; }
    operator const char*() const { return text.c_str(); }
};
std::map<std::string, Value> my;
std::vector<std::string> events;
unsigned long now = 10000, opened_at = 0;
int ir = HIGH, micro = HIGH, solenoid = LOW, sends = 0, opens = 0;
bool tag_active = false, ir_chip_pending = false;
bool ir_chip_confirm_animated = false;
bool transport_failed = false;
unsigned long millis() { return now; }
void pinMode(int pin, int mode) {
    assert(pin == SOLENOID_PIN && mode == OUTPUT);
    events.push_back("init");
}
int digitalRead(int pin) { return pin == IR_SENSOR_PIN ? ir : micro; }
void digitalWrite(int pin, int value) {
    assert(pin == SOLENOID_PIN);
    if (value == HIGH) {
        assert(solenoid == LOW);
        opened_at = now;
        ++opens;
        events.push_back("on");
    } else {
        if (solenoid == HIGH) assert(now - opened_at == 2000);
        events.push_back("off");
    }
    solenoid = value;
}
void delay(unsigned long ms) {
    events.push_back("delay:" + std::to_string(ms));
    now += ms;
}
struct Wifi {
    void Send(String device, const char* key, const char* value) {
        assert(solenoid == LOW);
        assert(device == "LA" && String(key) == "taken_chip" && String(value) == "+1");
        ++sends;
        events.push_back("send");
        // Model a slow request, including the library's void-return failure path.
        now += 5000;
        events.push_back(transport_failed ? "send_failed" : "send_returned");
    }
} has2wifi;
struct Log {
    void begin(int) {}
    template<class T> void println(T) {}
    template<class... T> void printf(const char*, T...) {}
} Serial, SerialMirror;
void OnTagChipConfirmed() {}
// Blackbox hooks: every record is requested while the solenoid is de-energized,
// and the count request is bracketed before the pulse. Logging never opens it.
std::vector<std::string> blackbox;
bool blackbox_started = false;
void BlackBoxBegin() {
    // The journal starts before the boot purge so the purge itself is recorded.
    assert(opens == 0 && solenoid == LOW && !blackbox_started);
    blackbox_started = true;
}
void BlackBoxAltar(const char* label, int32_t a, int32_t, int32_t, int32_t) {
    assert(solenoid == LOW);
    String name = label;
    assert(name.size() <= 27);
    if (name == "altar.count.begin") assert(blackbox.back() == "altar.sw.click");
    if (name == "altar.count.end") assert(a == 5000 && blackbox.back() == "altar.count.begin");
    if (name == "altar.solenoid.on") assert(a == SOLENOID_PULSE_MS);
    blackbox.push_back(name);
}
// LED blackout around the pulse: off must come before the (blocking) send and the pulse,
// restore must come after the pulse. No success sound: the DFPlayer was removed.
void NeoSceneOff() { events.push_back("leds_off"); }
void NeoSceneRestore() { assert(solenoid == LOW); events.push_back("leds_on"); }
void CrashReportInit() { events.push_back("crash_init"); }
// Normal boot: the OTA-only boot (no logger, no BLE) is decided before the purge.
bool ota_boot_checked = false;
void OtaBootInit() { assert(opens == 0 && !blackbox_started); ota_boot_checked = true; }
bool OtaLeanBoot() { assert(ota_boot_checked); return false; }
void LogMemoryStats(const char*) {}
void TempleInit() {
    // Boot purge must finish before Wi-Fi/RFID initialization can block.
    assert(opens == 1 && solenoid == LOW && sends == 0);
    events.push_back("temple_init");
}
void DataChange() { events.push_back("data_change"); }
'''

tests = r'''
void expect(std::initializer_list<std::string> expected) {
    assert(events == std::vector<std::string>(expected));
}
void expectBlackbox(std::initializer_list<std::string> expected) {
    assert(blackbox == std::vector<std::string>(expected));
}
void detectChip() {
    ir = LOW;
    IrSensorLoop();
    assert(ir_chip_pending);
}
void crank() { micro = LOW; MicroSwLoop(); }
int main(int argc, char** argv) {
    assert(argc == 2);
    String scenario = argv[1];
    my["device_name"] = "LA";
    my["game_state"] = "activate";
    my["device_state"] = "activate";
    if (scenario == "boot") {
        setup();
        expect({"init", "off", "delay:1000", "crash_init", "on", "delay:2000",
                "off", "temple_init", "data_change"});
        assert(sends == 0 && opens == 1 && !ir_chip_pending && !tag_active);
        assert(blackbox_started);
        expectBlackbox({"altar.solenoid.on", "altar.solenoid.off"});
    } else if (scenario == "no_chip") {
        tag_active = true;
        crank();
        expect({});
        assert(sends == 0 && opens == 0);
        // An ignored click is still recorded.
        expectBlackbox({"altar.sw.click"});
    } else if (scenario == "no_tag") {
        detectChip();
        crank();
        expect({});
        assert(sends == 0 && opens == 0 && ir_chip_pending);
        expectBlackbox({"altar.ir.chip", "altar.sw.click"});
    } else if (scenario == "setting" || scenario == "setting_stale_tag") {
        my["game_state"] = "setting";
        tag_active = scenario == "setting_stale_tag";
        detectChip();
        crank();
        expect({"leds_off", "on", "delay:2000", "off", "leds_on"});
        assert(sends == 0 && opens == 1 && !ir_chip_pending);
        expectBlackbox({"altar.ir.chip", "altar.sw.click", "altar.solenoid.on", "altar.solenoid.off"});
    } else {
        tag_active = true;
        transport_failed = scenario == "send_failed";
        if (scenario == "blink") my["device_state"] = "blink";
        detectChip();
        crank();
        if (transport_failed) {
            expect({"leds_off", "send", "send_failed", "on", "delay:2000", "off", "leds_on"});
        } else {
            expect({"leds_off", "send", "send_returned", "on", "delay:2000", "off", "leds_on"});
        }
        assert(sends == 1 && opens == 1 && !ir_chip_pending);
        expectBlackbox({"altar.ir.chip", "altar.sw.click", "altar.count.begin", "altar.count.end",
                        "altar.solenoid.on", "altar.solenoid.off"});
        if (scenario == "held_switch") {
            // A new chip while the crank switch stays pressed must not count again.
            ir = HIGH; IrSensorLoop();
            now += IR_SENSOR_DEBOUNCE_MS + 1;
            detectChip();
            for (int i = 0; i < 5; ++i) { now += 1000; MicroSwLoop(); }
            assert(sends == 1 && opens == 1 && ir_chip_pending);
            // No second click edge while the switch stays pressed.
            assert(blackbox.back() == "altar.ir.chip");
            micro = HIGH; MicroSwLoop();
            assert(blackbox.back() == "altar.sw.release");
            now += MICRO_SW_DEBOUNCE_MS + 1;
            crank();
            assert(sends == 2 && opens == 2 && !ir_chip_pending);
        }
    }
    assert(solenoid == LOW);
    std::cout << "PASS " << scenario << '\n';
}
'''

production = "\n".join(function("sensor.ino", name) for name in (
    "SolenoidInit", "SolenoidOn", "SolenoidOff", "SolenoidPulse",
    "RunAltarSuccess", "IrSensorLoop", "MicroSwLoop"))
production += "\n" + function("HAS1_altar.ino", "setup")
with tempfile.TemporaryDirectory(prefix="altar-solenoid-") as temp:
    source, executable = Path(temp) / "test.cpp", Path(temp) / "test"
    source.write_text(defines + "\n" + prefix + production + tests)
    subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(source), "-o", str(executable)], check=True)
    for scenario in ("boot", "activate", "blink", "setting", "setting_stale_tag",
                     "no_chip", "no_tag", "held_switch", "send_failed"):
        # Isolate production static debounce state for independent scenarios.
        subprocess.run([str(executable), scenario], check=True)
