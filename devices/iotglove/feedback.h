#pragma once

#include "feedback_config.h"

namespace iotglove {

enum class MotorSource : uint8_t { None, State, Event, Command, Proximity, Forced };

struct Outputs {
  uint8_t red = 0, green = 0, blue = 0;
  uint8_t lit = 0;
  bool motor = false;
  MotorSource motorSource = MotorSource::None;
  uint32_t motorPressId = 0;
};

class FeedbackEngine {
 public:
  explicit FeedbackEngine(const feedback_config::Settings& settings = feedback_config::Settings{})
      : settings_(settings) {}

  // The latest actually started Found pattern owns the quiet window, including
  // its OFF gap. Queue-head command deadlines cannot shorten a later pattern.
  bool foundPending(uint32_t now) const {
    return found_ && uint32_t(now - patternStart_) < pattern_.total;
  }
  uint32_t foundUntilMs() const { return patternStart_ + pattern_.total; }

  Outputs update(const Feedback& state, uint8_t vibe, bool locationFresh, uint32_t now,
                 bool suppressed = false) {
    Outputs out;
    out.lit = state.lit;
    switch (state.display) {
      case Display::Setting: out.red = out.green = out.blue = 32; break;
      case Display::Ready:
      case Display::Ended: out.red = 64; break;
      case Display::Player: out.green = 64; break;
      case Display::Ghost: out.blue = 64; break;
      case Display::Tagger:
      case Display::TaggerActive: out.red = 48; out.blue = 64; break;
      case Display::TaggerBlink:
        if (display_ != Display::TaggerBlink) blinkStart_ = now;
        if ((uint32_t(now - blinkStart_) / 500U) % 2U == 0) { out.red = 48; out.blue = 64; }
        break;
    }

    // Academy taggers are a fixed visual marker. Ignore state transitions,
    // proximity levels, operator vibe commands and pending event patterns.
    if (state.phase == Phase::Academy && state.deviceState == DeviceState::Tagger) {
      cancel();
      pendingGhostAck_ = false;
      known_ = state.stateValid;
      remember(state);
      if (state.stateValid) {
        lastVibe_ = vibe;
        haveVibe_ = true;
      }
      return out;
    }

    if (!state.stateValid || suppressed) {
      cancel();
      pendingGhostAck_ = false;
      known_ = state.stateValid;
      remember(state);
      // Suppressed (OTA/reset) polls consume a held command so it never plays late. Invalid polls
      // carry a forced vibe of 0 and leave lastVibe_ alone, so the next valid poll is not an edge.
      if (state.stateValid) {
        lastVibe_ = vibe;
        haveVibe_ = true;
      }
      return out;
    }
    const bool baseline = !known_ || state.stateEpoch != epoch_;
    const bool changed = !baseline && (state.role != role_ ||
        state.deviceState != deviceState_ || state.phase != phase_);
    const bool academyBoundary = (phase_ == Phase::Academy) !=
        (state.phase == Phase::Academy);
    const bool removedAcknowledged = changed && pendingGhostAck_ &&
        role_ == Role::Player && state.role == Role::Ghost &&
        state.deviceState == deviceState_ && state.phase == phase_;
    if (baseline) {
      cancel();
      pendingGhostAck_ = false;
      // A command already held at first sync or after an identity/epoch change is consumed, not
      // played. A baseline caused only by a transient invalid poll keeps lastVibe_, so a command
      // that changed meanwhile still plays.
      if (!haveVibe_ || state.stateEpoch != epoch_) lastVibe_ = vibe;
      haveVibe_ = true;
    }
    // No event, command or state pattern may cross the Academy boundary in
    // either direction. Repeated Academy player polls still leave their local
    // Removed/Found pattern running.
    if (academyBoundary) cancel();
    if (pattern_.total && uint32_t(now - patternStart_) >= pattern_.total) cancel();

    if (state.phase == Phase::Academy) {
      // Consume actual server values so commands held through Academy cannot
      // appear as a late edge after exit. Only local chip/button events play.
      lastVibe_ = vibe;
      haveVibe_ = true;
      if (state.haptic != Haptic::None) {
        const auto choice = state.haptic == Haptic::Removed ? settings_.onRemoved : settings_.onFound;
        start(choice, Source::Event, now);
        found_ = state.haptic == Haptic::Found && pattern_.total;
        patternPressId_ = state.hapticPressId;
      }
      known_ = true;
      remember(state);
      if (pattern_.total) {
        out.motor = feedback_config::motorOn(pattern_, uint32_t(now - patternStart_));
        annotate(out);
      }
      return out;
    }
    // Stop any preceding activity before a new setting/ready/end notification.
    // This is an edge, not a per-poll cancellation of the new notification.
    // Operator commands are not state feedback and outlive these transitions.
    if (source_ != Source::Command &&
        ((changed && quiescent(state)) ||
         (state.display == Display::Ended && display_ != Display::Ended))) cancel();
    if (changed) pendingGhostAck_ = false;

    // Operator vibe commands. 10/11 are levels that hold while the server keeps the value;
    // 12..17 play once per change of value (the server must pass through another value to repeat).
    const bool vibeEdge = vibe != lastVibe_;
    lastVibe_ = vibe;
    if (vibe == feedback_config::kVibeMute || vibe == feedback_config::kVibeOn) {
      if (vibeEdge) cancel();  // Nothing queued resumes once the level is released.
      known_ = true;
      remember(state);
      out.motor = vibe == feedback_config::kVibeOn;
      out.motorSource = MotorSource::Forced;
      return out;
    }
    if (vibeEdge && feedback_config::isCommand(vibe)) {
      start(feedback_config::commandSchedule(vibe, settings_), Source::Command, now);
    }

    if (state.haptic != Haptic::None) {
      const auto choice = state.haptic == Haptic::Removed ? settings_.onRemoved : settings_.onFound;
      start(choice, Source::Event, now);
      found_ = state.haptic == Haptic::Found && pattern_.total;
      patternPressId_ = state.hapticPressId;
      // A capture's later server acknowledgement must not repeat its vibration.
      if (state.haptic == Haptic::Removed && pattern_.total && state.role == Role::Player &&
          state.phase == Phase::Active) pendingGhostAck_ = true;
    } else if (changed && !academyBoundary && !removedAcknowledged && source_ != Source::Event &&
               source_ != Source::Command) {
      // Consume transitions during explicit events or commands; never queue a stale replay.
      start(feedback_config::forState(state, settings_), Source::State, now);
    }
    known_ = true;
    remember(state);

    if (pattern_.total) {
      // The OFF gap is part of the pattern and also overrides proximity pulses.
      out.motor = feedback_config::motorOn(pattern_, uint32_t(now - patternStart_));
      annotate(out);
    } else if (locationFresh &&
               (state.display == Display::Player || state.display == Display::Ghost)) {
      out.motorSource = MotorSource::Proximity;
      if (vibe == 3) {
        const uint32_t t = now % 1000U;
        out.motor = t < 100U || (t >= 200U && t < 300U);
      } else if (vibe == 1) out.motor = now % 2000U < 100U;
    }
    return out;
  }

 private:
  enum class Source : uint8_t { None, State, Event, Command };
  feedback_config::Settings settings_;
  feedback_config::Schedule pattern_;
  uint32_t patternStart_ = 0;
  uint32_t patternPressId_ = 0;
  bool found_ = false;
  uint32_t blinkStart_ = 0;
  Source source_ = Source::None;
  bool known_ = false;
  bool pendingGhostAck_ = false;
  Role role_ = Role::Neutral;
  DeviceState deviceState_ = DeviceState::Other;
  Phase phase_ = Phase::Unknown;
  Display display_ = Display::Setting;
  uint32_t epoch_ = 0;
  uint8_t lastVibe_ = 0;   // Last server vibe seen on a valid poll; commands play on change only.
  bool haveVibe_ = false;  // False until the first valid poll; baseline consumption depends on it.

  static bool quiescent(const Feedback& state) {
    return state.phase == Phase::Setting || state.phase == Phase::Ready ||
        state.phase == Phase::Photo || state.phase == Phase::Ended ||
        state.deviceState == DeviceState::Setting || state.deviceState == DeviceState::Ready ||
        state.deviceState == DeviceState::Photo || state.deviceState == DeviceState::Ended;
  }
  void annotate(Outputs& out) const {
    out.motorSource = static_cast<MotorSource>(source_);
    out.motorPressId = source_ == Source::Event ? patternPressId_ : 0;
  }
  void cancel() { pattern_ = feedback_config::Schedule{}; source_ = Source::None; patternPressId_ = 0; found_ = false; }
  void start(const feedback_config::Schedule& schedule, Source source, uint32_t now) {
    pattern_ = schedule;
    patternPressId_ = 0;
    found_ = false;
    source_ = pattern_.total ? source : Source::None;
    patternStart_ = now;
  }
  void start(feedback_config::Pattern pattern, Source source, uint32_t now) {
    start(feedback_config::schedule(pattern, settings_), source, now);
  }
  void remember(const Feedback& state) {
    role_ = state.role;
    deviceState_ = state.deviceState;
    phase_ = state.phase;
    display_ = state.display;
    epoch_ = state.stateEpoch;
  }
};

}  // namespace iotglove
