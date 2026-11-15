#include "../feedback.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace iotglove;
using feedback_config::Pattern;
using feedback_config::Settings;

static Feedback state(Role role, DeviceState device, Phase phase, Display display) {
  Feedback out;
  out.stateValid = true;
  out.role = role;
  out.deviceState = device;
  out.phase = phase;
  out.display = display;
  return out;
}

static void patternsAndConfiguration() {
  Settings config;
  auto pattern = feedback_config::schedule(Pattern::Short1, config);
  assert(pattern.total == 150 && feedback_config::motorOn(pattern, 149));
  assert(!feedback_config::motorOn(pattern, 150));
  pattern = feedback_config::schedule(Pattern::Long1, config);
  assert(pattern.total == 300 && feedback_config::motorOn(pattern, 299));
  assert(!feedback_config::motorOn(pattern, 300));
  pattern = feedback_config::schedule(Pattern::Short2, config);
  assert(pattern.total == 400 && feedback_config::motorOn(pattern, 149));
  assert(!feedback_config::motorOn(pattern, 150) && !feedback_config::motorOn(pattern, 249));
  assert(feedback_config::motorOn(pattern, 250) && feedback_config::motorOn(pattern, 399));
  assert(!feedback_config::motorOn(pattern, 400));
  assert(feedback_config::schedule(Pattern::Off, config).total == 0);
  config.shortMs = 10; config.doubleGapMs = 5; config.longMs = 40;
  pattern = feedback_config::schedule(Pattern::Short2, config);
  assert(pattern.total == 25 && !feedback_config::motorOn(pattern, 10));
  assert(feedback_config::motorOn(pattern, 15) && !feedback_config::motorOn(pattern, 25));
  config.doubleGapMs = 0;
  assert(feedback_config::schedule(Pattern::Short2, config).total == 20);
  config.shortMs = 0;
  assert(feedback_config::schedule(Pattern::Short1, config).total == 0);
  assert(feedback_config::schedule(Pattern::Short2, config).total == 0);
  config.longMs = 0;
  assert(feedback_config::schedule(Pattern::Long1, config).total == 0);
  config.shortMs = UINT32_MAX; config.longMs = UINT32_MAX;
  assert(feedback_config::schedule(Pattern::Short2, config).total == 0);
  assert(feedback_config::schedule(Pattern::Long1, config).total == 0);

  config = Settings{};
  Feedback f = state(Role::Tagger, DeviceState::Blink, Phase::Ready, Display::TaggerBlink);
  assert(feedback_config::forState(f, config) == config.onTaggerBlink);
  f.deviceState = DeviceState::Activate;
  assert(feedback_config::forState(f, config) == config.onTaggerActive);
  f.deviceState = DeviceState::Setting;
  assert(feedback_config::forState(f, config) == config.onSetting);
  f.deviceState = DeviceState::Ready;
  assert(feedback_config::forState(f, config) == config.onReady);
  f.phase = Phase::Photo;
  assert(feedback_config::forState(f, config) == config.onPhoto);
  f.phase = Phase::Ended;
  assert(feedback_config::forState(f, config) == config.onEnded);
  f = state(Role::Ghost, DeviceState::Other, Phase::Active, Display::Ghost);
  assert(feedback_config::forState(f, config) == config.onGhost);
  f.role = Role::Player;
  assert(feedback_config::forState(f, config) == config.onPlayer);
  f.role = Role::Neutral;
  assert(feedback_config::forState(f, config) == Pattern::Off);
}

static void semanticTransitionsAndNoReplay() {
  FeedbackEngine engine;
  auto f = state(Role::Neutral, DeviceState::Setting, Phase::Setting, Display::Setting);
  assert(!engine.update(f, 0, false, 0).motor);  // First sync is a baseline.
  f.deviceState = DeviceState::Ready; f.phase = Phase::Ready; f.display = Display::Ready;
  assert(engine.update(f, 0, false, 1000).motor);
  assert(engine.update(f, 0, false, 1149).motor);
  assert(!engine.update(f, 0, false, 1150).motor);  // Repeated polls do not restart.
  f.lit = 3;
  assert(!engine.update(f, 0, false, 2000).motor);  // Chip/count display changes are not states.
  f.role = Role::Tagger; f.deviceState = DeviceState::Blink; f.display = Display::TaggerBlink;
  assert(engine.update(f, 0, false, 3000).motor);
  assert(!engine.update(f, 0, false, 3150).motor);
  assert(engine.update(f, 0, false, 3250).motor);
  assert(!engine.update(f, 0, false, 3400).motor);
  f.deviceState = DeviceState::Activate; f.display = Display::TaggerActive;
  assert(engine.update(f, 0, false, 4000).motor);
  assert(engine.update(f, 0, false, 4299).motor);
  assert(!engine.update(f, 0, false, 4300).motor);
  f = state(Role::Ghost, DeviceState::Other, Phase::Active, Display::Ghost);
  assert(engine.update(f, 0, false, 5000).motor);
  f.role = Role::Player; f.display = Display::Player;  // Role color is independent of the chip.
  assert(engine.update(f, 0, false, 6000).motor);
  assert(!engine.update(f, 0, false, 6150).motor);
  f.display = Display::Player; f.lit = 4;
  assert(!engine.update(f, 0, false, 6200).motor);
  f.stateEpoch++;
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(!engine.update(f, 0, false, 6300).motor);  // Reconnect/identity replacement.
  f.stateValid = false;
  assert(!engine.update(f, 3, true, 6400).motor);
  f.stateValid = true; f.role = Role::Player; f.display = Display::Player;
  assert(!engine.update(f, 0, false, 6500).motor);
  f.phase = Phase::Ended; f.display = Display::Ended;
  assert(engine.update(f, 0, false, 7000).motor);
  assert(engine.update(f, 0, false, 7299).motor);
  assert(!engine.update(f, 0, false, 7300).motor);
}

static void prioritiesAndCancellation() {
  Settings config;
  config.onPlayer = Pattern::Short2;
  FeedbackEngine engine(config);
  auto f = state(Role::Ghost, DeviceState::Other, Phase::Active, Display::Ghost);
  engine.update(f, 0, false, 0);
  f.role = Role::Player; f.display = Display::Player;
  assert(engine.update(f, 3, true, 1000).motor);
  assert(!engine.update(f, 3, true, 1200).motor);  // State OFF gap beats proximity ON.
  f.haptic = Haptic::Found;
  assert(engine.update(f, 3, true, 1250).motor);  // Explicit event replaces state pattern.
  f.haptic = Haptic::None;
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(!engine.update(f, 0, false, 1400).motor);  // Role change cannot interrupt event gap.
  assert(engine.update(f, 0, false, 1500).motor);
  assert(!engine.update(f, 0, false, 1650).motor);
  assert(!engine.update(f, 0, false, 1800).motor);  // No queued transition replay.
  f.haptic = Haptic::Removed;
  assert(engine.update(f, 0, false, 2000).motor);
  f.haptic = Haptic::None;
  f.phase = Phase::Ready; f.display = Display::Ready;
  assert(engine.update(f, 0, false, 2001).motor);  // Ready cancels old event, starts its own short pulse.
  assert(!engine.update(f, 0, false, 2151).motor);
  f.phase = Phase::Active; f.display = Display::Ghost;
  assert(engine.update(f, 0, false, 3000).motor);
  f.stateValid = false;
  assert(!engine.update(f, 0, false, 3001).motor);
  f.stateValid = true;
  assert(!engine.update(f, 0, false, 3002).motor);
  f.haptic = Haptic::Found;
  assert(!engine.update(f, 0, false, 4000, true).motor);  // OTA/reset consumes and cancels.
  f.haptic = Haptic::None; f.role = Role::Player; f.display = Display::Player;
  assert(!engine.update(f, 0, false, 4001, true).motor);
  assert(!engine.update(f, 0, false, 4002).motor);
}

static void onlyServerRoleTransitionsSignal() {
  GameModel game(Profile::Origin);
  FeedbackEngine engine;
  game.begin(true, 0);
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.deviceState = DeviceState::Activate;
  s.role = Role::Player; s.stepSeconds = 3;
  strcpy(s.session, "run1"); strcpy(s.deviceName, "G1P1");
  game.applyServer(s, 0);
  assert(!engine.update(game.feedback(), 0, false, 0).motor);
  game.chipChanged(false, 10);
  auto out = engine.update(game.feedback(), 0, false, 10);
  assert(out.green == 64 && out.lit == 4 && !out.motor);
  game.chipChanged(true, 20);
  assert(!engine.update(game.feedback(), 0, false, 20).motor);
  s.role = Role::Ghost; s.revivalCount = 2; game.applyServer(s, 1000);
  out = engine.update(game.feedback(), 0, false, 1000);
  assert(out.blue == 64 && out.lit == 2 && out.motor);
  game.applyServer(s, 1100);
  assert(engine.update(game.feedback(), 0, false, 1100).motor);
  assert(!engine.update(game.feedback(), 0, false, 1300).motor);
  game.chipChanged(false, 1400);
  assert(!engine.update(game.feedback(), 0, false, 1400).motor);
  s.role = Role::Player; game.applyServer(s, 2000);
  out = engine.update(game.feedback(), 0, false, 2000);
  assert(out.green == 64 && out.lit == 4 && out.motor);
  assert(!engine.update(game.feedback(), 0, false, 2150).motor);
}

static void wrapAndTaggerLeds() {
  FeedbackEngine engine;
  auto f = state(Role::Tagger, DeviceState::Blink, Phase::Ready, Display::TaggerBlink);
  const uint32_t start = UINT32_MAX - 200U;
  auto out = engine.update(f, 0, false, start);
  assert(out.red == 48 && out.blue == 64 && out.lit == 4 && !out.motor);
  out = engine.update(f, 0, false, 298);
  assert(out.red == 48 && out.blue == 64);  // 499 ms across wrap.
  out = engine.update(f, 0, false, 299);
  assert(out.red == 0 && out.blue == 0);
  out = engine.update(f, 0, false, 799);
  assert(out.red == 48 && out.blue == 64);
  f.deviceState = DeviceState::Activate; f.display = Display::TaggerActive;
  out = engine.update(f, 0, false, 800);
  assert(out.red == 48 && out.blue == 64 && out.motor);
  out = engine.update(f, 0, false, 1500);
  assert(out.red == 48 && out.blue == 64 && !out.motor);  // Activate never blinks.
  f.display = Display::Tagger;
  out = engine.update(f, 0, false, 1700);
  assert(out.red == 48 && out.blue == 64);

  FeedbackEngine wrap;
  f = state(Role::Player, DeviceState::Other, Phase::Active, Display::Player);
  wrap.update(f, 0, false, start - 1);
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(wrap.update(f, 0, false, start).motor);
  assert(wrap.update(f, 0, false, 98).motor);
  assert(!wrap.update(f, 0, false, 99).motor);  // 300 ms transition pulse across wrap.
}

static void playerAndGhostProximity() {
  const Feedback participants[] = {
      state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player),
      state(Role::Ghost, DeviceState::Activate, Phase::Active, Display::Ghost),
  };
  for (auto f : participants) {
    FeedbackEngine engine;
    // Player and Ghost (including the server's revival alias) share the same pulses.
    assert(engine.update(f, 3, true, 0).motor);
    assert(engine.update(f, 3, true, 99).motor);
    assert(!engine.update(f, 3, true, 100).motor);
    assert(!engine.update(f, 3, true, 199).motor);
    assert(engine.update(f, 3, true, 200).motor);
    assert(engine.update(f, 3, true, 299).motor);
    assert(!engine.update(f, 3, true, 300).motor);
    assert(engine.update(f, 3, true, 1000).motor);
    assert(engine.update(f, 1, true, 2000).motor);
    assert(engine.update(f, 1, true, 2099).motor);
    assert(!engine.update(f, 1, true, 2100).motor);
    assert(!engine.update(f, 1, true, 3000).motor);
    assert(engine.update(f, 1, true, 4000).motor);
    assert(!engine.update(f, 0, true, 4001).motor);
    assert(!engine.update(f, 2, true, 4002).motor);
    assert(!engine.update(f, 1, false, 4003).motor);
    assert(!engine.update(f, 3, false, 4004).motor);
    assert(!engine.update(f, 10, true, 4005).motor);
    assert(!engine.update(f, 3, true, 4006, true).motor);
    f.stateValid = false;
    assert(!engine.update(f, 3, true, 4007).motor);
    f.stateValid = true;
    f.phase = Phase::Academy;
    assert(!engine.update(f, 3, true, 4008).motor);

    FeedbackEngine ready;
    f.phase = Phase::Ready;
    f.deviceState = DeviceState::Ready;
    f.display = Display::Ready;
    assert(!ready.update(f, 3, true, 5000).motor);
  }
}

static void commandSchedules() {
  Settings config;
  auto s = feedback_config::pulses(200, 200, 3);
  assert(s.total == 1000 && s.count == 3 && s.onMs == 200 && s.gapMs == 200);
  assert(feedback_config::motorOn(s, 0) && feedback_config::motorOn(s, 199));
  assert(!feedback_config::motorOn(s, 200) && !feedback_config::motorOn(s, 399));
  assert(feedback_config::motorOn(s, 400) && feedback_config::motorOn(s, 599));
  assert(!feedback_config::motorOn(s, 600) && !feedback_config::motorOn(s, 799));
  assert(feedback_config::motorOn(s, 800) && feedback_config::motorOn(s, 999));
  assert(!feedback_config::motorOn(s, 1000));
  assert(feedback_config::pulses(0, 200, 3).total == 0);       // Zero ON disables.
  assert(feedback_config::pulses(200, 200, 0).total == 0);     // Zero count disables.
  assert(feedback_config::pulses(UINT32_MAX, 0, 2).total == 0);  // Overflow rejected.
  assert(feedback_config::pulses(300, 0, 1).total == 300);

  assert(feedback_config::commandSchedule(12, config).total == 200);
  assert(feedback_config::commandSchedule(13, config).total == 600);
  assert(feedback_config::commandSchedule(14, config).total == 1000);
  assert(feedback_config::commandSchedule(15, config).total == 600);
  assert(feedback_config::commandSchedule(16, config).total == 1400);
  assert(feedback_config::commandSchedule(17, config).total == 2200);
  assert(feedback_config::commandSchedule(0, config).total == 0);
  assert(feedback_config::commandSchedule(11, config).total == 0);
  assert(feedback_config::commandSchedule(18, config).total == 0);
  assert(feedback_config::isCommand(12) && feedback_config::isCommand(17));
  assert(!feedback_config::isCommand(11) && !feedback_config::isCommand(18));
  assert(feedback_config::kVibeMute == 10 && feedback_config::kVibeOn == 11);

  // Existing state patterns keep their exact shape on the generalized schedule.
  s = feedback_config::schedule(Pattern::Short2, config);
  assert(s.onMs == 150 && s.gapMs == 100 && s.count == 2 && s.total == 400);
  s = feedback_config::schedule(Pattern::Long1, config);
  assert(s.onMs == 300 && s.gapMs == 0 && s.count == 1 && s.total == 300);
}

static void commandEdgesAndLevels() {
  FeedbackEngine engine;
  auto f = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  assert(!engine.update(f, 0, false, 0).motor);       // Baseline.
  assert(engine.update(f, 12, false, 1000).motor);    // 0 -> 12 edge: short x1 (200 ms).
  assert(engine.update(f, 12, false, 1199).motor);
  assert(!engine.update(f, 12, false, 1200).motor);
  assert(!engine.update(f, 12, false, 2000).motor);   // A held value never restarts.
  assert(engine.update(f, 13, false, 3000).motor);    // 12 -> 13 edge: short x2.
  assert(!engine.update(f, 13, false, 3200).motor);
  assert(engine.update(f, 13, false, 3400).motor);
  assert(!engine.update(f, 13, false, 3600).motor);
  assert(!engine.update(f, 0, false, 4000).motor);    // Back to 0: silence.
  assert(engine.update(f, 13, false, 5000).motor);    // Same command replays after a 0 gap.
  assert(!engine.update(f, 13, false, 5600).motor);
  assert(engine.update(f, 15, false, 6000).motor);    // Long x1: 600 ms.
  assert(engine.update(f, 15, false, 6599).motor);
  assert(!engine.update(f, 15, false, 6600).motor);
  assert(!engine.update(f, 7, false, 7000).motor);    // 4..9 behave like 0.
  assert(!engine.update(f, 2, false, 7100).motor);

  assert(engine.update(f, 11, false, 8000).motor);    // 11 holds the motor on...
  assert(engine.update(f, 11, false, 9000).motor);
  assert(!engine.update(f, 0, false, 10000).motor);   // ...until released.
  assert(engine.update(f, 17, false, 11000).motor);   // Long x3 running (ON 11000-11599, 11800-12399)...
  assert(!engine.update(f, 10, false, 11100).motor);  // ...mute cancels it.
  assert(!engine.update(f, 10, false, 11500).motor);
  assert(!engine.update(f, 0, false, 11900).motor);   // Released mute: the cancelled train does not resume.
  assert(engine.update(f, 14, false, 12000).motor);   // Short x3 (ON 12000-12199, 12400-12599, 12800-12999)...
  assert(engine.update(f, 11, false, 12100).motor);   // ...replaced by continuous ON...
  assert(!engine.update(f, 0, false, 12450).motor);   // ...and it does not resume when ON is released.
}

static void commandPrioritiesAndExceptions() {
  FeedbackEngine engine;
  auto f = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  engine.update(f, 0, false, 0);  // Baseline.

  // Chip events interrupt a running command: a game signal beats an operator cue.
  assert(engine.update(f, 14, false, 1000).motor);    // Short x3: ON 1000-1199, 1400-1599, 1800-1999.
  f.haptic = Haptic::Removed;
  assert(engine.update(f, 14, false, 1250).motor);    // Removed (Long1, 300 ms) starts inside the command's OFF gap.
  f.haptic = Haptic::None;
  assert(engine.update(f, 14, false, 1549).motor);
  assert(!engine.update(f, 14, false, 1550).motor);   // Event done; the interrupted command does not resume (1550 was an ON window).

  // Role transitions do not cut a running command.
  assert(engine.update(f, 16, false, 3000).motor);    // Long x2: ON 3000-3599, 3800-4399.
  f.role = Role::Ghost; f.display = Display::Ghost;
  assert(!engine.update(f, 16, false, 3650).motor);   // Gap holds; the Ghost transition pulse is not started.
  assert(engine.update(f, 16, false, 3800).motor);
  assert(engine.update(f, 16, false, 4399).motor);
  assert(!engine.update(f, 16, false, 4400).motor);

  // Quiescent (setting/ready/ended) transitions do not cut a running command either.
  f = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  engine.update(f, 0, false, 5000);                   // Player transition pulse (150 ms) plays and ends.
  assert(!engine.update(f, 0, false, 5500).motor);
  assert(engine.update(f, 17, false, 6000).motor);    // Long x3: ON 6000-6599, 6800-7399, 7600-8199.
  f.deviceState = DeviceState::Ready; f.phase = Phase::Ready; f.display = Display::Ready;
  assert(!engine.update(f, 17, false, 6700).motor);   // Ready did not cut it (its own pulse would be ON now).
  assert(engine.update(f, 17, false, 6900).motor);    // The command's second pulse continues.
  assert(!engine.update(f, 17, false, 8200).motor);

  // Commands work in any state. Invalid polls carry a forced vibe of 0 and must not create edges:
  // the same command afterwards stays silent, a changed one still plays.
  assert(engine.update(f, 12, false, 9000).motor);    // Ready state, command plays.
  assert(!engine.update(f, 12, false, 9200).motor);
  f.stateValid = false;
  assert(!engine.update(f, 0, false, 9500).motor);
  f.stateValid = true;
  assert(!engine.update(f, 12, false, 10000).motor);  // Same 12 after the blip: no replay.
  f.stateValid = false;
  engine.update(f, 0, false, 10500);
  f.stateValid = true;
  assert(engine.update(f, 13, false, 11000).motor);   // Changed during the blip: plays.
  assert(!engine.update(f, 13, false, 11600).motor);

  // OTA/reset consumes commands instead of replaying them afterwards.
  assert(!engine.update(f, 14, false, 12000, true).motor);
  assert(!engine.update(f, 14, false, 12100).motor);  // Released: 14 was consumed.
  assert(engine.update(f, 15, false, 12500).motor);   // A new edge after release plays normally.

  // A command already held at first sync or after an epoch change is consumed, not played.
  FeedbackEngine fresh;
  auto g = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  assert(!fresh.update(g, 12, false, 0).motor);
  assert(!fresh.update(g, 12, false, 500).motor);
  g.stateEpoch++;                                     // Reconnect / identity replacement while 13 is held.
  assert(!fresh.update(g, 13, false, 1000).motor);
  assert(!fresh.update(g, 13, false, 1100).motor);
  assert(fresh.update(g, 12, false, 2000).motor);     // 13 -> 12 after the baseline is a real edge.

  // Mute silences game feedback too: proximity, chip events and transitions.
  FeedbackEngine muted;
  auto m = state(Role::Player, DeviceState::Activate, Phase::Active, Display::Player);
  muted.update(m, 0, true, 0);
  assert(muted.update(m, 3, true, 1000).motor);       // Same-room proximity pulse (t % 1000 < 100).
  assert(!muted.update(m, 10, true, 2000).motor);     // Muted inside a proximity ON window.
  m.haptic = Haptic::Removed;
  assert(!muted.update(m, 10, true, 2050).motor);     // Chip event swallowed.
  m.haptic = Haptic::None;
  m.deviceState = DeviceState::Ready; m.phase = Phase::Ready; m.display = Display::Ready;
  assert(!muted.update(m, 10, true, 2100).motor);     // Ready transition swallowed.
  m.deviceState = DeviceState::Activate; m.phase = Phase::Active; m.display = Display::Player;
  assert(!muted.update(m, 10, true, 2200).motor);     // Player transition swallowed.
  assert(muted.update(m, 3, true, 3000).motor);       // Released: proximity is back, no stale transition replays.
}

int main() {
  patternsAndConfiguration();
  semanticTransitionsAndNoReplay();
  prioritiesAndCancellation();
  onlyServerRoleTransitionsSignal();
  wrapAndTaggerLeds();
  playerAndGhostProximity();
  commandSchedules();
  commandEdgesAndLevels();
  commandPrioritiesAndExceptions();
  puts("PASS: configured state haptics, priorities, reconnect suppression, authoritative roles, tagger LEDs and operator vibe commands");
}
