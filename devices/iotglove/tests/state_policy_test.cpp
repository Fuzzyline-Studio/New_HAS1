#include "state_policy.h"
#include "network_policy.h"
#include "ota_request.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
using namespace iotglove;

int main() {
  ServerSnapshot s;
  s.valid = true; s.role = Role::Ghost; s.lifeChip = 1;
  strcpy(s.session, "current"); strcpy(s.deviceName, "G1P1");
  GameEvent event; event.kind = GameEvent::Kind::SetCount;
  strcpy(event.session, s.session); strcpy(event.deviceName, s.deviceName);
  for (const char* device : {"setting", "ready"}) {
    assert(decodeServerStates("activate", device, s));
    assert(s.phase == Phase::Active);  // Display preparation never permits live OTA.
    assert(!otaAllowedPhase(s.phase));
    assert(!gameMutationsAllowed(s));
    assert(!commandAllowed(event, s) && !commandApplied(event, s));
  }
  assert(decodeServerStates("ready", "activate", s));
  assert(s.phase == Phase::Ready && s.deviceState == DeviceState::Activate);
  assert(!gameMutationsAllowed(s) && !commandAllowed(event, s));
  assert(decodeServerStates("activate", "blink", s));
  assert(s.deviceState == DeviceState::Blink && gameMutationsAllowed(s));
  assert(decodeServerStates("activate", "activate", s));
  assert(s.deviceState == DeviceState::Activate && commandAllowed(event, s));
  for (const char* device : {"player", "tagger"}) {
    assert(decodeServerStates("academy", device, s));
    assert(s.phase == Phase::Academy && !gameMutationsAllowed(s));
    assert(!commandAllowed(event, s) && !commandApplied(event, s));
  }
  assert(!decodeServerStates("academy", "activate", s));
  assert(!decodeServerStates("activate", "player", s));
  assert(!decodeServerStates("ready", "tagger", s));
  for (const char* game : {"setting", "ready", "activate", "stop", "end"}) {
    assert(decodeServerStates(game, "photo", s));
    assert(s.phase == Phase::Photo && s.deviceState == DeviceState::Photo);
    assert(!commandAllowed(event, s) && !otaAllowedPhase(s.phase));
  }
  // The removed exploration alias is rejected so a legacy value cannot reopen
  // active-game mutations while the application waits for a valid snapshot.
  assert(!decodeServerStates("activate", "exploration", s));
  for (const char* device : {"win", "lose", "stop"}) {
    assert(decodeServerStates("activate", device, s));
    assert(s.phase == Phase::Ended && !commandAllowed(event, s));
  }
  for (const char* device : {"github", "github@12:7", "b", "reset", "unknown"}) {
    assert(decodeServerStates("activate", device, s));
    assert(s.phase == Phase::Active && s.deviceState == DeviceState::Other);
    assert(!otaAllowedPhase(s.phase));
  }
  assert(!decodeServerStates("bogus", "activate", s));
  assert(!decodeServerStates(nullptr, "ready", s));
  assert(!decodeServerStates("ready", nullptr, s));
  assert(decodeServerStates("stop", "blink", s));
  assert(s.phase == Phase::Ended);
  assert(!strcmp(deviceStateName(DeviceState::Blink), "blink"));
  assert(!strcmp(deviceStateName(DeviceState::Photo), "photo"));
  assert(!strcmp(deviceStateName(DeviceState::Player), "player"));
  assert(!strcmp(deviceStateName(DeviceState::Tagger), "tagger"));
  puts("PASS: production state decoding, preparation mutation gates and OTA phase isolation");
}
