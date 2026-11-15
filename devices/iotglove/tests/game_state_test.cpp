#include "../game_state.h"
#include "../feedback.h"
#include "../network_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

using namespace iotglove;

static ServerSnapshot snapshot(Role role = Role::Ghost) {
  ServerSnapshot s;
  s.valid = true;
  strcpy(s.session, "G1:run1");
  strcpy(s.deviceName, "G1P1");
  s.phase = Phase::Active;
  s.role = role;
  s.stepSeconds = 3;
  return s;
}

static GameEvent pop(GameModel& game, GameEvent::Kind kind, int value = -1) {
  GameEvent event;
  assert(game.peekEvent(event));
  assert(event.kind == kind);
  if (value >= 0) assert(event.value == value);
  assert(strcmp(event.deviceName, "G1P1") == 0);
  game.consumeEvent();
  return event;
}

static void noEvent(GameModel& game) {
  GameEvent event;
  assert(!game.peekEvent(event));
}

static void trainingBoundaries() {
  GameModel g(Profile::Training);
  g.begin(true, 100);
  g.tick(100);
  assert(g.feedback().display == Display::Player);
  g.chipChanged(false, 200);
  Feedback f = g.feedback();
  assert(f.display == Display::Ghost && f.lit == 1 && f.haptic == Haptic::Removed);
  g.tick(3199); assert(g.count() == 1);
  g.tick(3200); assert(g.count() == 2);
  g.chipChanged(true, 5000);
  g.tick(6200); assert(g.count() == 3);
  g.tick(9199); assert(g.feedback().display == Display::Ghost);
  g.tick(9200); assert(g.feedback().display == Display::Player);
  noEvent(g);
  g.chipChanged(false, 10000);
  g.tick(19000); assert(g.count() == 4 && g.feedback().display == Display::Ghost);
  g.chipChanged(true, 19001); assert(g.feedback().display == Display::Player);
}

static void trainingResetAndBoot() {
  GameModel g(Profile::Training);
  g.begin(false, UINT32_MAX - 1000U);
  g.tick(UINT32_MAX - 1000U);
  assert(g.feedback().display == Display::Ghost && g.count() == 1);
  g.tick(1999); assert(g.count() == 2);  // unsigned millis rollover
  g.chipChanged(true, 2500);
  g.buttonPressed(2600);
  assert(g.feedback().haptic == Haptic::Found);
  g.tick(11599); assert(g.feedback().display == Display::Ghost);
  g.tick(11600); assert(g.feedback().display == Display::Player);
  g.buttonPressed(12000); assert(g.feedback().haptic == Haptic::None);
  g.chipChanged(false, 13000);
  g.chipChanged(true, 15000);
  g.chipChanged(false, 16000);
  g.tick(19000); assert(g.count() == 2);
}

static ServerSnapshot academy(DeviceState device) {
  auto s = snapshot(Role::Neutral);
  s.phase = Phase::Academy;
  s.deviceState = device;
  return s;
}

static void runtimeAcademyPlayerUsesTrainingRules() {
  GameModel g(Profile::Origin);
  FeedbackEngine engine;
  g.begin(true, 0);
  g.applyServer(academy(DeviceState::Player), 100);
  g.tick(100);
  auto f = g.feedback();
  assert(f.phase == Phase::Academy && f.deviceState == DeviceState::Player);
  assert(f.display == Display::Player && f.lit == 4);
  engine.update(f, 0, false, 100);  // Establish the server-state baseline.

  g.chipChanged(false, 200);
  f = g.feedback();
  assert(f.display == Display::Ghost && f.lit == 1 && f.haptic == Haptic::Removed);
  auto out = engine.update(f, 0, false, 200);
  assert(out.blue == 64 && out.lit == 1 && out.motor);
  g.tick(3199); assert(g.count() == 1);
  g.tick(3200); assert(g.count() == 2);
  g.buttonPressed(4000);
  g.tick(4000);
  f = g.feedback();
  assert(f.haptic == Haptic::Found && f.lit == 1);
  g.chipChanged(true, 5000);
  g.tick(12999); assert(g.feedback().display == Display::Ghost);
  g.tick(13000); assert(g.feedback().display == Display::Player);
  noEvent(g);  // Academy never writes revival_count to the server.
}

static void runtimeAcademyTaggerIsFixedAndSilent() {
  GameModel g(Profile::Origin);
  FeedbackEngine engine;
  g.begin(true, 0);
  g.applyServer(academy(DeviceState::Player), 10);
  g.chipChanged(false, 20);  // Queue an explicit training haptic before mode change.
  g.applyServer(academy(DeviceState::Tagger), 30);
  auto f = g.feedback();
  assert(f.phase == Phase::Academy && f.deviceState == DeviceState::Tagger);
  assert(f.display == Display::Tagger && f.lit == 4 && f.haptic == Haptic::None);
  auto out = engine.update(f, feedback_config::kVibeOn, true, 30);
  assert(out.red == 48 && out.green == 0 && out.blue == 64 && out.lit == 4);
  assert(!out.motor);

  g.chipChanged(true, 40);
  g.chipChanged(false, 50);
  g.buttonPressed(60);
  g.tick(10000);
  f = g.feedback();
  out = engine.update(f, feedback_config::kVibeCommandLast, true, 10000);
  assert(f.display == Display::Tagger && f.lit == 4 && f.haptic == Haptic::None);
  assert(!out.motor);
  noEvent(g);
}

static void runtimeAcademyTransitionsResetLocalTrainingState() {
  GameModel g(Profile::Origin);
  FeedbackEngine engine;
  g.begin(true, 0);
  auto livePlayer = snapshot(Role::Player);
  livePlayer.deviceState = DeviceState::Activate;
  g.applyServer(livePlayer, 1);
  engine.update(g.feedback(), 0, false, 1);
  auto originOut = engine.update(g.feedback(), feedback_config::kVibeCommandFirst, false, 2);
  assert(originOut.motor);

  g.applyServer(academy(DeviceState::Player), 10);
  auto academyOut = engine.update(g.feedback(), 0, false, 10);
  assert(!academyOut.motor);  // An Origin operator pattern cannot leak into Academy.
  g.chipChanged(false, 20);
  auto removed = g.feedback();
  assert(removed.haptic == Haptic::Removed);
  academyOut = engine.update(removed, 0, false, 20);
  assert(academyOut.motor);  // Local Academy input haptics still run.
  academyOut = engine.update(g.feedback(), 0, false, 21);
  assert(academyOut.motor);  // The explicit pattern is not cancelled by the next poll.
  g.tick(9020);
  assert(g.count() == 4 && g.feedback().display == Display::Ghost);

  // Changing Academy device type clears the player's timer and any event.
  g.applyServer(academy(DeviceState::Tagger), 9030);
  assert(g.count() == 0 && g.feedback().haptic == Haptic::None);
  g.tick(20000);
  assert(g.feedback().display == Display::Tagger && g.count() == 0);

  // Re-entering player starts from the current physical chip, not stale time.
  g.applyServer(academy(DeviceState::Player), 20010);
  assert(g.count() == 1 && g.feedback().display == Display::Ghost);
  g.tick(23009); assert(g.count() == 1);
  g.tick(23010); assert(g.count() == 2);

  // Leaving Academy clears local haptics/timers and restores authoritative data.
  auto live = snapshot(Role::Ghost);
  live.deviceState = DeviceState::Activate;
  live.revivalCount = 3;
  g.buttonPressed(23020);
  g.applyServer(live, 23030);
  auto f = g.feedback();
  assert(f.phase == Phase::Active && f.display == Display::Ghost && f.lit == 3);
  assert(f.haptic == Haptic::None && g.count() == 3);
}

static void runtimeAcademyConsumesVibeAndCancelsExitHaptic() {
  {
    GameModel g(Profile::Origin);
    FeedbackEngine engine;
    g.begin(true, 0);
    auto academyPlayer = academy(DeviceState::Player);
    academyPlayer.vibe = feedback_config::kVibeCommandFirst;
    g.applyServer(academyPlayer, 10);
    auto out = engine.update(g.feedback(), academyPlayer.vibe, false, 10);
    assert(!out.motor);  // Academy consumes, but never executes, operator commands.

    g.chipChanged(false, 20);
    out = engine.update(g.feedback(), academyPlayer.vibe, false, 20);
    assert(out.motor);  // The local Removed event is still allowed.

    auto live = snapshot(Role::Player);
    live.deviceState = DeviceState::Activate;
    live.vibe = academyPlayer.vibe;
    g.applyServer(live, 21);
    out = engine.update(g.feedback(), live.vibe, false, 21);
    assert(!out.motor);  // Event pattern and unchanged command both stop at exit.
    out = engine.update(g.feedback(), live.vibe, false, 22);
    assert(!out.motor);

    out = engine.update(g.feedback(), 0, false, 30);
    assert(!out.motor);
    out = engine.update(g.feedback(), feedback_config::kVibeCommandFirst, false, 31);
    assert(out.motor);  // A real post-Academy edge remains usable.
  }

  {
    GameModel g(Profile::Origin);
    FeedbackEngine engine;
    g.begin(true, 0);
    auto academyPlayer = academy(DeviceState::Player);
    academyPlayer.vibe = feedback_config::kVibeOn;
    g.applyServer(academyPlayer, 10);
    assert(!engine.update(g.feedback(), academyPlayer.vibe, false, 10).motor);

    auto live = snapshot(Role::Player);
    live.deviceState = DeviceState::Activate;
    live.vibe = feedback_config::kVibeOn;
    g.applyServer(live, 20);
    // Vibe 11 is a live ON level, so it takes effect immediately in Origin.
    assert(engine.update(g.feedback(), live.vibe, false, 20).motor);
    assert(engine.update(g.feedback(), live.vibe, false, 21).motor);
    engine.update(g.feedback(), 0, false, 30);
    assert(engine.update(g.feedback(), feedback_config::kVibeOn, false, 31).motor);
  }

  for (uint8_t proximity : {uint8_t(1), uint8_t(3)}) {
    GameModel g(Profile::Origin);
    FeedbackEngine engine;
    g.begin(true, 0);
    auto academyPlayer = academy(DeviceState::Player);
    academyPlayer.vibe = proximity;
    g.applyServer(academyPlayer, 10);
    assert(!engine.update(g.feedback(), proximity, true, 10).motor);

    auto live = snapshot(Role::Player);
    live.deviceState = DeviceState::Activate;
    live.vibe = proximity;
    g.applyServer(live, 20);
    // Proximity is a live level, not an operator command to suppress after Academy.
    assert(engine.update(g.feedback(), proximity, true, 20).motor);
  }
}

static void chipChangesNeverRequestRolesOrLife() {
  for (Role role : {Role::Neutral, Role::Player, Role::Ghost, Role::Tagger}) {
    for (DeviceState device : {DeviceState::Other, DeviceState::Setting, DeviceState::Ready,
         DeviceState::Blink, DeviceState::Activate, DeviceState::Photo, DeviceState::Ended}) {
      GameModel g(Profile::Origin);
      g.begin(true, 0);
      auto s = snapshot(role); s.deviceState = device; s.lifeChip = 1;
      g.applyServer(s, 10);
      g.chipChanged(false, 20);
      assert(!g.chipPresent() && g.server().role == role && g.server().lifeChip == 1);
      assert(g.feedback().haptic == Haptic::None); noEvent(g);
      g.chipChanged(true, 30);
      assert(g.chipPresent() && g.server().role == role && g.server().lifeChip == 1);
      assert(g.feedback().haptic == Haptic::None); noEvent(g);
      s.valid = false; g.applyServer(s, 40);
      g.chipChanged(false, 50);
      assert(!g.chipPresent()); noEvent(g);  // Observation still updates while offline.
    }
  }
}

static void playerColorWaitsForAuthoritativeRole() {
  GameModel g(Profile::Origin);
  FeedbackEngine engine;
  g.begin(true, 0);
  auto s = snapshot(Role::Player);
  s.deviceState = DeviceState::Activate;
  g.applyServer(s, 10);
  auto out = engine.update(g.feedback(), 0, false, 10);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  g.chipChanged(false, 20);
  noEvent(g);
  out = engine.update(g.feedback(), 0, false, 20);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  g.applyServer(s, 30);  // Physical removal cannot override the server role.
  out = engine.update(g.feedback(), 0, false, 30);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  g.chipChanged(true, 40);
  out = engine.update(g.feedback(), 0, false, 40);
  assert(out.green == 64 && out.blue == 0 && out.lit == 4);
  s.role = Role::Ghost; s.revivalCount = 2;
  g.applyServer(s, 50);
  out = engine.update(g.feedback(), 0, false, 50);
  assert(out.red == 0 && out.green == 0 && out.blue == 64 && out.lit == 2);
  noEvent(g);
  g.chipChanged(false, 60); noEvent(g);
  s.role = Role::Player; g.applyServer(s, 70);
  out = engine.update(g.feedback(), 0, false, 70);
  assert(out.red == 0 && out.green == 64 && out.blue == 0 && out.lit == 4);
  noEvent(g);  // Server revival while chip is absent never triggers client recapture.
}

static void countAndServerRevival() {
  GameModel g(Profile::Origin);
  g.begin(false, 0);
  auto s = snapshot();
  g.applyServer(s, 100);
  g.tick(3100); g.tick(60000); assert(g.count() == 0);
  noEvent(g);  // The server charges revival_count; the glove never counts on its own.
  s.revivalCount = 1; g.applyServer(s, 3500); assert(g.count() == 1);
  s.revivalCount = 2; g.applyServer(s, 6500); assert(g.count() == 2);
  s.revivalCount = 3; g.applyServer(s, 9500); assert(g.count() == 3);
  g.tick(15100); noEvent(g);
  assert(!g.canUseLifeDevice());
  s.revivalCount = 4; s.sacrificed = true;
  g.applyServer(s, 15110); assert(g.count() == 4);
  noEvent(g);
  assert(g.canUseLifeDevice());
  g.chipChanged(true, 18200);
  noEvent(g);  // Physical observation does not mutate life or role.
  s.revivalCount = 4; s.open = true;
  g.applyServer(s, 18300);
  g.tick(18300);
  noEvent(g);  // Even all revival conditions wait for a server role transition.
  assert(g.feedback().display == Display::Ghost && !g.canUseLifeDevice());
  s.role = Role::Player; g.applyServer(s, 18400);
  assert(g.feedback().display == Display::Player);
  noEvent(g);
}

static void foundRetainsChipAndOpen() {
  GameModel g(Profile::Origin);
  g.begin(false, 0);
  auto s = snapshot(); s.sacrificed = true; s.open = true; s.revivalCount = 2;
  g.applyServer(s, 100);
  g.chipChanged(true, 200); noEvent(g);
  g.buttonPressed(300); pop(g, GameEvent::Kind::SetCount, 1);
  assert(g.chipPresent() && !g.canUseLifeDevice() && g.count() == 1);
  s.revivalCount = 1; g.applyServer(s, 500);
  g.tick(12300); assert(g.count() == 1);
  noEvent(g);  // Automatic revival belongs to the server.
}

static void rebootRestoreAndPendingCountOrder() {
  GameModel g(Profile::Origin);
  g.begin(true, 0);
  auto s = snapshot();
  s.open = true; s.sacrificed = true; s.lifeChip = 1; s.revivalCount = 3;
  g.applyServer(s, 100);
  noEvent(g);  // no new physical insertion delta after reboot
  g.tick(3100); assert(g.count() == 3);
  noEvent(g);  // Automatic revival belongs to the server.

  GameModel resetting(Profile::Origin);
  resetting.begin(false, 0);
  s.open = false; s.lifeChip = 0; s.revivalCount = 2;
  resetting.applyServer(s, 0);
  // The button writes the server minimum 1, never 0.
  resetting.buttonPressed(3100); pop(resetting, GameEvent::Kind::SetCount, 1);
  assert(resetting.count() == 1);
  s.revivalCount = 3; resetting.applyServer(s, 3200);
  assert(resetting.count() == 1);  // an older read cannot undo the pending button write
  s.revivalCount = 1; resetting.applyServer(s, 3300);  // read-back acknowledges the write
  assert(resetting.count() == 1);
  s.revivalCount = 2; resetting.applyServer(s, 6300); assert(resetting.count() == 2);
  s.revivalCount = 3; resetting.applyServer(s, 9300); assert(resetting.count() == 3);
  resetting.tick(20000); assert(resetting.count() == 3);
  noEvent(resetting);  // Later server charging is followed, never written back.

  // Pressing at the server minimum: an old read of 1 settles early and harmlessly.
  GameModel minimum(Profile::Origin);
  minimum.begin(false, 0);
  s.revivalCount = 1; minimum.applyServer(s, 0);
  minimum.buttonPressed(100); pop(minimum, GameEvent::Kind::SetCount, 1);
  minimum.applyServer(s, 200); assert(minimum.count() == 1);
  s.revivalCount = 2; minimum.applyServer(s, 3200); assert(minimum.count() == 2);
}

static void roleChangeDropsQueuedCounts() {
  GameModel g(Profile::Origin); g.begin(false, 0);
  auto s = snapshot(Role::Ghost); g.applyServer(s, 0);
  g.buttonPressed(3000);
  GameEvent count; assert(g.peekEvent(count) && count.value == 1);
  s.role = Role::Player; g.applyServer(s, 3100);
  noEvent(g); assert(g.count() == 0);
  g.tick(10000); g.buttonPressed(10001); noEvent(g);
  s.role = Role::Ghost; g.applyServer(s, 11000);
  g.tick(14000); noEvent(g);
  g.buttonPressed(14001); pop(g, GameEvent::Kind::SetCount, 1);
}

static void remoteChangesAndFailures() {
  GameModel g(Profile::Origin);
  g.begin(false, UINT32_MAX - 1000U);
  auto s = snapshot(); s.sacrificed = true;
  g.applyServer(s, UINT32_MAX - 1000U);
  g.tick(1999); noEvent(g);
  s.revivalCount = 1; g.applyServer(s, 2000);
  s.stepSeconds = 1; g.applyServer(s, 2500);
  g.tick(3500); assert(g.count() == 1); noEvent(g);
  s.revivalCount = 2; g.applyServer(s, 3500);
  g.buttonPressed(3500); auto event = pop(g, GameEvent::Kind::SetCount, 1);
  g.commandUncertain(event.sequence);
  assert(!g.synchronized());
  assert(g.feedback().display == Display::Ghost);  // Loss of sync preserves the last color.
  g.buttonPressed(3600); noEvent(g);
  g.applyServer(s, 3700); assert(g.count() == 2 && g.synchronized());
  g.buttonPressed(3800);
  strcpy(s.session, "G1:run2");
  s.phase = Phase::Photo;
  s.deviceState = DeviceState::Photo;
  g.applyServer(s, 3900);
  noEvent(g); assert(g.feedback().display == Display::Player);  // Photo treats ghosts as survivors.
  g.chipChanged(true, 4000); noEvent(g);
  s.phase = Phase::Ended; s.role = Role::Tagger;
  g.applyServer(s, 4100); assert(g.feedback().display == Display::Ended);
  s.valid = false;
  g.applyServer(s, 4101);
  assert(!g.synchronized() && !g.server().valid);
  g.chipChanged(false, 4102); noEvent(g);
}

static void debounceAndOverflow() {
  DebouncedInput input;
  input.begin(false, UINT32_MAX - 20U);
  assert(!input.update(true, UINT32_MAX - 10U));
  assert(!input.update(false, UINT32_MAX - 5U));
  assert(!input.update(true, 0));
  assert(!input.update(true, 29));
  assert(input.update(true, 30));
  assert(!input.update(true, 100));
  GameModel g(Profile::Origin);
  g.begin(false, 0);
  auto s = snapshot(); s.sacrificed = true;
  g.applyServer(s, 0);
  for (unsigned n = 1; n < 15; ++n) g.buttonPressed(n);
  assert(g.queueOverflowed() && !g.synchronized());
  noEvent(g);
  g.applyServer(s, 100);
  assert(g.synchronized() && !g.queueOverflowed());
  noEvent(g);
}

static void explicitPreparationStates() {
  for (Role role : {Role::Neutral, Role::Player, Role::Ghost, Role::Tagger}) {
    for (Phase phase : {Phase::Setting, Phase::Ready, Phase::Active}) {
      for (DeviceState device : {DeviceState::Setting, DeviceState::Ready}) {
        GameModel g(Profile::Origin);
        g.begin(false, 0);
        auto s = snapshot(role);
        s.phase = phase; s.deviceState = device; s.connectionEpoch = 7;
        s.lifeChip = 1; s.sacrificed = true; s.open = true;
        g.applyServer(s, 10);
        auto f = g.feedback();
        assert(f.display == (device == DeviceState::Setting ? Display::Setting : Display::Ready));
        assert(f.lit == 3 && f.stateValid && f.stateEpoch == 7);
        assert(f.role == role && f.phase == phase && f.deviceState == device);
        g.chipChanged(true, 20);
        assert(g.feedback().lit == 4);
        g.chipChanged(false, 30);
        g.buttonPressed(40); g.tick(10000);
        assert(g.feedback().lit == 3 && g.count() == 0);
        noEvent(g);  // Preparation edges/timers never mutate life_chip or role.
      }
    }
  }
  // A display change out of live play discards an unsubmitted count safely.
  GameModel g(Profile::Origin); g.begin(false, 0);
  auto s = snapshot(Role::Ghost); s.deviceState = DeviceState::Activate;
  g.applyServer(s, 0); g.buttonPressed(3000);
  GameEvent count; assert(g.peekEvent(count) && count.kind == GameEvent::Kind::SetCount);
  s.deviceState = DeviceState::Ready; g.applyServer(s, 3010);
  noEvent(g); assert(g.feedback().haptic == Haptic::None);
  s.deviceState = DeviceState::Activate; g.applyServer(s, 3020);
  noEvent(g);  // Returning to active cannot replay the earlier queued count.
}

static void taggerDisplayAndSafety() {
  for (Phase phase : {Phase::Setting, Phase::Ready, Phase::Active}) {
    GameModel g(Profile::Origin); g.begin(false, 0);
    auto s = snapshot(Role::Tagger); s.phase = phase;
    s.deviceState = DeviceState::Blink; s.connectionEpoch = 1;
    g.applyServer(s, 10);
    auto f = g.feedback();
    assert(f.display == Display::TaggerBlink && f.lit == 4 && f.stateValid);
    s.deviceState = DeviceState::Activate; g.applyServer(s, 20);
    f = g.feedback(); assert(f.display == Display::TaggerActive && f.stateEpoch == 1);
    s.deviceState = DeviceState::Other; g.applyServer(s, 30);
    assert(g.feedback().display != Display::TaggerBlink);  // Unknown is not blink.
    noEvent(g);
  }
  GameModel g(Profile::Origin); g.begin(true, 0);
  auto s = snapshot(Role::Tagger); s.deviceState = DeviceState::Blink;
  s.connectionEpoch = 9;
  g.applyServer(s, 0); assert(g.feedback().display == Display::TaggerBlink);
  s.valid = false; g.applyServer(s, 10);
  auto f = g.feedback(); assert(!f.stateValid && f.display == Display::TaggerBlink);
  s.valid = true; s.connectionEpoch = 10; g.applyServer(s, 20);
  f = g.feedback(); assert(f.stateValid && f.stateEpoch == 10);
  s.phase = Phase::Ended; g.applyServer(s, 30);
  f = g.feedback(); assert(f.display == Display::Ended);
  g.chipChanged(false, 40); g.buttonPressed(50); g.tick(10000); noEvent(g);
}

static void invalidStateKeepsLastColor() {
  for (Role role : {Role::Player, Role::Ghost, Role::Tagger}) {
    GameModel g(Profile::Origin);
    FeedbackEngine engine;
    g.begin(true, 0);
    auto s = snapshot(role);
    s.deviceState = DeviceState::Activate;
    if (role == Role::Ghost) s.revivalCount = 2;
    g.applyServer(s, 10);
    auto before = engine.update(g.feedback(), 0, false, 10);
    if (role == Role::Player)
      assert(before.red == 0 && before.green == 64 && before.blue == 0 && before.lit == 4);
    else if (role == Role::Ghost)
      assert(before.red == 0 && before.green == 0 && before.blue == 64 && before.lit == 2);
    else
      assert(before.red == 48 && before.green == 0 && before.blue == 64 && before.lit == 4);

    auto invalid = s;
    invalid.valid = false;
    g.applyServer(invalid, 20);
    auto feedback = g.feedback();
    auto after = engine.update(feedback, 0, false, 20);
    assert(!feedback.stateValid);
    assert(after.red == before.red && after.green == before.green && after.blue == before.blue);
    assert(after.lit == before.lit);

    s.connectionEpoch++;
    g.applyServer(s, 30);
    before = engine.update(g.feedback(), 0, false, 30);
    g.commandUncertain(0);  // Same path used by the 15-second freshness timeout.
    feedback = g.feedback();
    after = engine.update(feedback, 0, false, 40);
    assert(!feedback.stateValid);
    assert(after.red == before.red && after.green == before.green && after.blue == before.blue);
    assert(after.lit == before.lit);
  }
}

static void photoUsesRoleColors() {
  for (Role role : {Role::Neutral, Role::Player, Role::Ghost, Role::Tagger}) {
    GameModel g(Profile::Origin);
    FeedbackEngine engine;
    g.begin(false, 0);
    auto s = snapshot(role);
    s.phase = Phase::Photo;
    s.deviceState = DeviceState::Photo;
    if (role == Role::Ghost) s.revivalCount = 2;
    g.applyServer(s, 10);
    auto feedback = g.feedback();
    auto out = engine.update(feedback, 0, false, 10);
    assert(feedback.display == (role == Role::Tagger ? Display::Tagger : Display::Player));
    assert(out.lit == 4);
    if (role == Role::Tagger) assert(out.red == 48 && out.green == 0 && out.blue == 64);
    else assert(out.red == 0 && out.green == 64 && out.blue == 0);
    g.buttonPressed(20); g.tick(10000); noEvent(g);  // Photo remains mutation-safe.
  }
}

static void taggerBlinkContinuesWhileInvalid() {
  GameModel g(Profile::Origin);
  FeedbackEngine engine;
  g.begin(true, 0);
  auto s = snapshot(Role::Tagger);
  s.deviceState = DeviceState::Blink;
  g.applyServer(s, 0);
  auto out = engine.update(g.feedback(), 0, false, 0);
  assert(out.red == 48 && out.blue == 64);

  s.valid = false;
  g.applyServer(s, 10);
  out = engine.update(g.feedback(), 0, false, 10);
  assert(out.red == 48 && out.blue == 64);
  out = engine.update(g.feedback(), 0, false, 500);
  assert(out.red == 0 && out.green == 0 && out.blue == 0);
  out = engine.update(g.feedback(), 0, false, 1000);
  assert(out.red == 48 && out.blue == 64);
}

static void buttonWaitsForFoundPattern() {
  const feedback_config::Settings settings;
  const uint32_t found = feedback_config::schedule(settings.onFound, settings).total;
  assert(found == 400);  // 150/100/150 ms
  for (uint32_t at : {1000U, UINT32_MAX - 100U}) {
    GameModel g(Profile::Origin);
    g.begin(false, at);
    g.applyServer(snapshot(), at);
    assert(g.buttonPressed(at).disposition == ButtonDisposition::Queued);
    GameEvent e; assert(g.peekEvent(e) && e.value == 1);
    assert(e.notBeforeMs == uint32_t(at + found));
    assert(!eventDue(e, at) && !eventDue(e, at + found - 1));
    assert(eventDue(e, at + found) && eventDue(e, at + found + 60000));
    assert(g.feedback().haptic == Haptic::Found);  // Local feedback is immediate.
  }
}

int main() {
  trainingBoundaries();
  trainingResetAndBoot();
  runtimeAcademyPlayerUsesTrainingRules();
  runtimeAcademyTaggerIsFixedAndSilent();
  runtimeAcademyTransitionsResetLocalTrainingState();
  runtimeAcademyConsumesVibeAndCancelsExitHaptic();
  chipChangesNeverRequestRolesOrLife();
  playerColorWaitsForAuthoritativeRole();
  countAndServerRevival();
  roleChangeDropsQueuedCounts();
  foundRetainsChipAndOpen();
  rebootRestoreAndPendingCountOrder();
  remoteChangesAndFailures();
  debounceAndOverflow();
  explicitPreparationStates();
  taggerDisplayAndSafety();
  invalidStateKeepsLastColor();
  photoUsesRoleColors();
  taggerBlinkContinuesWhileInvalid();
  buttonWaitsForFoundPattern();
  puts("PASS: production game model, debounce, training, count, reconciliation and rollover");
}
