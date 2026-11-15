#include "../button_trace.h"
#include "../feedback.h"
#include "../network_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;
static ServerSnapshot server(Role role = Role::Ghost) {
  ServerSnapshot s;
  s.valid = true; s.phase = Phase::Active; s.role = role;
  s.deviceState = DeviceState::Activate;
  s.stepSeconds = 3;
  strcpy(s.session, "game:1"); strcpy(s.deviceName, "G1P1");
  return s;
}
static void decisionsAndIdentity() {
  GameModel game(Profile::Origin); game.begin(true, 0);
  assert(game.buttonPressed(1, 11, true).disposition == ButtonDisposition::Unsynchronized);
  game.applyServer(server(Role::Player), 2);
  assert(game.buttonPressed(3, 12, true).disposition == ButtonDisposition::NotGhost);
  game.applyServer(server(Role::Tagger), 4);
  assert(game.buttonPressed(5, 13, true).disposition == ButtonDisposition::NotGhost);
  assert(game.queuedEvents() == 0 && game.feedback().haptic == Haptic::None);
  game.applyServer(server(), 6);
  auto d = game.buttonPressed(7, 14, true);
  assert(d.disposition == ButtonDisposition::Queued && d.sequence != 0);
  GameEvent e; assert(game.peekEvent(e));
  assert(e.sequence == d.sequence && e.pressId == 14 && e.traceButton && e.value == 1);
  const auto f = game.feedback();
  assert(f.haptic == Haptic::Found && f.hapticPressId == 14);
  assert(game.feedback().hapticPressId == 0);
  game.consumeEvent();
  game.tick(3007); assert(!game.peekEvent(e)); // The server charges; no timer events.
  for (unsigned i = 0; i < 12; ++i)
    assert(game.buttonPressed(4000+i, 20+i, false).disposition == ButtonDisposition::Queued);
  assert(game.buttonPressed(5000, 99, true).disposition == ButtonDisposition::QueueFull);
  assert(game.queueOverflowed() && !game.synchronized() && game.queuedEvents() == 12);
  assert(game.buttonPressed(5001, 100, true).disposition == ButtonDisposition::Unsynchronized);
  GameModel training(Profile::Training); training.begin(false, 0);
  assert(training.buttonPressed(20, 101, true).disposition == ButtonDisposition::LocalAccepted);
  assert(training.queuedEvents() == 0 && training.feedback().hapticPressId == 101);
  GameModel academy(Profile::Origin); academy.begin(false, 0);
  auto a = server(Role::Neutral); a.phase = Phase::Academy; a.deviceState = DeviceState::Player;
  academy.applyServer(a, 1);
  assert(academy.buttonPressed(20, 102, true).disposition == ButtonDisposition::LocalAccepted);
  assert(academy.queuedEvents() == 0 && academy.feedback().hapticPressId == 102);
}
static void boundedInputsAndDebounce() {
  DebouncedInput input; input.begin(false, 0);
  assert(!input.update(true, 1)); assert(!input.update(false, 10));
  assert(!input.update(true, 20)); assert(!input.update(true, 49));
  assert(input.update(true, 50)); assert(!input.update(true, 999));
  assert(!input.update(false, 1000)); assert(input.update(false, 1030));
  ButtonTrace trace;
  for (unsigned i = 1; i <= 7; ++i) {
    assert(trace.press(i) == i);
    assert(trace.selected() == (i <= 4));
    assert(trace.suppressed(i) == 0);
  }
  assert(trace.suppressed(10001) == 3); // Summary appears even if chatter stops.
  assert(trace.suppressed(10002) == 0);
  assert(trace.press(10003) == 8 && trace.selected());
  TraceBudget wrap;
  assert(wrap.take(UINT32_MAX-10,1,100));
  assert(!wrap.take(10,1,100)); assert(wrap.take(100,1,100));
  assert(wrap.takeSuppressed(100,100) == 1);
}
static void realMotorEdgesAndSources() {
  GameModel game(Profile::Origin); game.begin(true, 0); game.applyServer(server(), 1);
  FeedbackEngine engine; engine.update(game.feedback(),0,false,1);
  MotorTrace trace; assert(trace.observe(1,false));
  game.buttonPressed(100, 77, true);
  auto out = engine.update(game.feedback(),0,false,100);
  assert(out.motor && out.motorSource == MotorSource::Event && out.motorPressId == 77);
  assert(trace.observe(100,out.motor));
  for (uint32_t t = 101; t < 250; ++t) {
    out=engine.update(game.feedback(),0,false,t); assert(!trace.observe(t,out.motor));
  }
  out=engine.update(game.feedback(),0,false,250);
  assert(!out.motor && out.motorPressId == 77 && trace.observe(250,out.motor));
  out=engine.update(game.feedback(),0,false,350);
  assert(out.motor && out.motorPressId == 77 && trace.observe(350,out.motor));
  out=engine.update(game.feedback(),0,false,500);
  assert(!out.motor && out.motorPressId == 0 && trace.observe(500,out.motor));
  // No 500 ms output gate can swallow the 150/100/150 pulse edges.
  game.buttonPressed(600,78,true);
  out=engine.update(game.feedback(),11,true,600);
  assert(out.motor && out.motorSource == MotorSource::Forced && out.motorPressId == 0);
  game.buttonPressed(700,79,true);
  out=engine.update(game.feedback(),10,true,700);
  assert(!out.motor && out.motorPressId == 0); // Muted button does not invent a motor event.
  out=engine.update(game.feedback(),3,true,1000);
  assert(out.motor && out.motorSource == MotorSource::Proximity && out.motorPressId == 0);
  MotorTrace noise;
  unsigned accepted=0;
  for (unsigned i=0;i<50;++i) accepted += noise.observe(i, (i%2)==0);
  assert(accepted==16 && noise.suppressed(1000)==34);
  assert(noise.suppressed(1001)==0);
}
static void observationalParity() {
  // Replay the same gameplay with tracing enabled/disabled: metadata must not
  // affect state, admission, timer progress, haptic waveforms or queue order.
  GameModel plain(Profile::Origin), traced(Profile::Origin);
  FeedbackEngine a,b;
  plain.begin(true,0); traced.begin(true,0);
  plain.applyServer(server(),1); traced.applyServer(server(),1);
  for (uint32_t t=1; t<20000; ++t) {
    if (t%731==0) { plain.buttonPressed(t); traced.buttonPressed(t,t,true); }
    plain.tick(t); traced.tick(t);
    auto x=a.update(plain.feedback(),0,false,t), y=b.update(traced.feedback(),0,false,t);
    assert(x.motor==y.motor && x.red==y.red && x.green==y.green && x.blue==y.blue && x.lit==y.lit);
    assert(plain.count()==traced.count() && plain.synchronized()==traced.synchronized());
    if (t%1000==0) {
      GameEvent p,q; bool have=plain.peekEvent(p); assert(have==traced.peekEvent(q));
      if (have) { assert(p.sequence==q.sequence && p.value==q.value); plain.consumeEvent(); traced.consumeEvent(); }
    }
  }
}
static void foundQuietQueueIntegration(uint32_t base, unsigned presses) {
  GameModel game(Profile::Origin); game.begin(false, base); game.applyServer(server(), base);
  FeedbackEngine engine; engine.update(game.feedback(), 0, false, base);
  uint32_t workerQuietUntil = base;
  unsigned submitted = 0;
  const uint32_t finish = 100U + (presses - 1U) * 300U + 400U;
  for (uint32_t elapsed = 1; elapsed <= finish + 20U; ++elapsed) {
    const uint32_t now = base + elapsed;
    // Production order: input, render/publish quiet, then command admission.
    if (elapsed >= 100 && (elapsed - 100) % 300 == 0 && (elapsed - 100) / 300 < presses) {
      const unsigned id = 1 + (elapsed - 100) / 300;
      assert(game.buttonPressed(now, id, true).disposition == ButtonDisposition::Queued);
    }
    const auto out = engine.update(game.feedback(), 0, false, now);
    if (engine.foundPending(now)) workerQuietUntil = engine.foundUntilMs();
    GameEvent head;
    if (game.peekEvent(head)) {
      if (elapsed >= 500 && elapsed < finish) {
        assert(eventDue(head, now)); // The oldest command is ready, but the latest pattern is not.
        assert(head.pressId == 1 && engine.foundPending(now));
      }
      if (eventDue(head, now) && !engine.foundPending(now)) {
        assert(!out.motor && !pendingUntil(workerQuietUntil, now, kNetworkQuietMaxMs));
        assert(elapsed >= finish && head.pressId == ++submitted);
        game.consumeEvent();
      }
    }
    if (elapsed >= 100 && elapsed < finish) {
      assert(engine.foundPending(now));
      assert(pendingUntil(workerQuietUntil, now, kNetworkQuietMaxMs));
      if (elapsed == 650 && presses >= 2) assert(out.motor && out.motorPressId == 2);
    }
  }
  assert(submitted == presses && game.queuedEvents() == 0);
  assert(!engine.foundPending(base + finish + 10000)); // No input: no indefinite quiet.
}
static void actualFoundPlaybackOwnsQuiet() {
  GameModel game(Profile::Origin); game.begin(false, 0); game.applyServer(server(), 0);
  FeedbackEngine engine;
  assert(!engine.foundPending(UINT32_MAX)); // An unset pattern is never a wraparound deadline.
  engine.update(game.feedback(), 0, false, 0);
  game.buttonPressed(100, 1, true);
  engine.update(game.feedback(), 0, false, 200); // Playback starts after the input timestamp.
  GameEvent head; assert(game.peekEvent(head) && head.notBeforeMs == 500);
  assert(engine.foundUntilMs() == 600);
  assert(eventDue(head, 500) && engine.foundPending(500));
  assert(!engine.foundPending(600));
  game.buttonPressed(700, 2, true);
  engine.update(game.feedback(), 10, false, 700); // Muted input never starts a Found window.
  assert(!engine.foundPending(700));
  game.buttonPressed(800, 3, true);
  engine.update(game.feedback(), 0, false, 800, true); // OTA/reset suppression consumes the event.
  assert(!engine.foundPending(800));
  engine.update(game.feedback(), 0, false, 801);
  assert(!engine.foundPending(801));
  game.buttonPressed(900, 4, true);
  engine.update(game.feedback(), 0, false, 900);
  assert(engine.foundPending(900));
  engine.update(game.feedback(), 0, false, 901, true);
  assert(!engine.foundPending(901));
  feedback_config::Settings off; off.onFound = feedback_config::Pattern::Off;
  FeedbackEngine disabled(off);
  game.buttonPressed(1000, 5, true);
  disabled.update(game.feedback(), 0, false, 1000);
  assert(!disabled.foundPending(1000));
  // Local Training and Academy events still play without a network command.
  const Profile profiles[] = {Profile::Training, Profile::Origin};
  for (const auto profile : profiles) {
    GameModel local(profile); local.begin(false, 0);
    if (profile == Profile::Origin) {
      auto s = server(Role::Neutral); s.phase = Phase::Academy; s.deviceState = DeviceState::Player;
      local.applyServer(s, 0);
    }
    FeedbackEngine feedback; feedback.update(local.feedback(), 0, false, 0);
    assert(local.buttonPressed(100).disposition == ButtonDisposition::LocalAccepted);
    assert(feedback.update(local.feedback(), 0, false, 100).motor);
    assert(feedback.foundPending(499) && !feedback.foundPending(500));
    assert(local.queuedEvents() == 0);
  }
}
int main() {
  decisionsAndIdentity(); boundedInputsAndDebounce(); realMotorEdgesAndSources(); observationalParity();
  for (unsigned presses = 1; presses <= 3; ++presses) {
    foundQuietQueueIntegration(0, presses);
    foundQuietQueueIntegration(UINT32_MAX - 799U, presses); // Two presses end exactly at millis()==0.
  }
  actualFoundPlaybackOwnsQuiet();
  puts("PASS: button decisions, causal IDs, debounce, bounded chatter, full queue, complete motor pulses and gameplay parity");
}
