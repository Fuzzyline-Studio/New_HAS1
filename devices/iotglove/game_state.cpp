#include "game_state.h"
#include "feedback_config.h"
#include "state_policy.h"

#include <string.h>

namespace iotglove {
namespace {
// A button command reaches the network only after its Found pattern has played.
const uint32_t kFoundMs = feedback_config::schedule(feedback_config::Settings{}.onFound,
                                                    feedback_config::Settings{}).total;
}  // namespace

void DebouncedInput::begin(bool level, uint32_t now) {
  stable_ = candidate_ = level;
  candidateSince_ = now;
}

bool DebouncedInput::update(bool level, uint32_t now) {
  if (level != candidate_) {
    candidate_ = level;
    candidateSince_ = now;
  }
  if (stable_ == candidate_ || uint32_t(now - candidateSince_) < intervalMs_) return false;
  stable_ = candidate_;
  return true;
}

void GameModel::resetQueue() {
  head_ = size_ = 0;
  countPending_ = false;
}

void GameModel::begin(bool chipPresent, uint32_t now) {
  chipPresent_ = chipPresent;
  trainingGhost_ = !chipPresent;  // A missing chip at boot must not show green.
  trainingStart_ = now;
  count_ = 0;
  haveServer_ = needsSync_ = overflow_ = false;
  haptic_ = Haptic::None;
  hapticPressId_ = 0;
  resetQueue();
}

bool GameModel::emit(GameEvent::Kind kind, uint8_t value, uint32_t pressId, bool trace) {
  if (needsSync_ || !haveServer_) return false;
  if (size_ == kCapacity) {
    // Stop producing mutations; keep existing ordered events for diagnostics.
    overflow_ = needsSync_ = true;
    return false;
  }
  GameEvent& event = queue_[(head_ + size_) % kCapacity];
  event = GameEvent{};
  event.kind = kind;
  event.value = value;
  event.pressId = pressId;
  event.traceButton = trace;
  event.sequence = ++sequence_;
  memcpy(event.session, server_.session, sizeof(event.session));
  memcpy(event.deviceName, server_.deviceName, sizeof(event.deviceName));
  ++size_;
  return true;
}

bool GameModel::peekEvent(GameEvent& out) const {
  if (!size_ || needsSync_) return false;
  out = queue_[head_];
  return true;
}

void GameModel::consumeEvent() {
  if (size_) { head_ = (head_ + 1) % kCapacity; --size_; }
}

void GameModel::commandUncertain(uint32_t /* sequence */) {
  // The network adapter follows this with a fresh authoritative snapshot.
  // Discard queued count writes until an authoritative snapshot reconciles them.
  needsSync_ = true;
  resetQueue();
}

bool GameModel::activeGhost() const {
  return haveServer_ && gameMutationsAllowed(server_) && server_.role == Role::Ghost;
}

bool GameModel::academyPlayer() const {
  return synchronized() && server_.valid && server_.phase == Phase::Academy &&
      server_.deviceState == DeviceState::Player;
}

void GameModel::applyServer(const ServerSnapshot& snapshot, uint32_t now) {
  if (profile_ == Profile::Training) return;
  if (!snapshot.valid) {
    needsSync_ = true;
    server_.valid = false;
    haptic_ = Haptic::None;
    hapticPressId_ = 0;
    resetQueue();
    return;
  }
  const bool newSession = !haveServer_ ||
      strcmp(snapshot.session, server_.session) != 0 ||
      strcmp(snapshot.deviceName, server_.deviceName) != 0;
  const bool phaseChanged = haveServer_ && snapshot.phase != server_.phase;
  const bool roleChanged = haveServer_ && snapshot.role != server_.role;
  const bool controlChanged = haveServer_ &&
      gameMutationsAllowed(snapshot) != gameMutationsAllowed(server_);
  const bool academyModeChanged = haveServer_ && snapshot.phase == Phase::Academy &&
      snapshot.deviceState != server_.deviceState;
  const bool recovering = needsSync_;
  if (newSession || phaseChanged || controlChanged || roleChanged || academyModeChanged || recovering) {
    resetQueue();
    haptic_ = Haptic::None;
    hapticPressId_ = 0;
    needsSync_ = overflow_ = false;
  }
  const uint8_t receivedCount = snapshot.revivalCount > 4 ? 4 : snapshot.revivalCount;
  if (snapshot.phase == Phase::Academy) {
    const bool startPlayer = snapshot.deviceState == DeviceState::Player &&
        (newSession || phaseChanged || academyModeChanged || recovering);
    server_ = snapshot;
    haveServer_ = true;
    if (startPlayer) {
      trainingGhost_ = !chipPresent_;
      trainingStart_ = now;
      count_ = trainingGhost_ ? 1 : 4;
    } else if (snapshot.deviceState == DeviceState::Tagger) {
      trainingGhost_ = false;
      count_ = 0;
    }
    return;
  }
  // The server charges the count. A pending button write keeps its local value
  // until read back; older reads cannot roll it back, later charges are followed.
  const bool countAcknowledged = countPending_ && receivedCount == count_;
  if (newSession || phaseChanged || controlChanged || roleChanged || recovering ||
      (receivedCount != observedCount_ && !countPending_)) {
    count_ = receivedCount;
    countPending_ = false;
  } else if (countAcknowledged) {
    countPending_ = false;
  }
  observedCount_ = receivedCount;
  server_ = snapshot;
  haveServer_ = true;
}

void GameModel::chipChanged(bool present, uint32_t now) {
  if (chipPresent_ == present) return;
  chipPresent_ = present;
  if (profile_ == Profile::Training || academyPlayer()) {
    if (!present) {
      trainingGhost_ = true;
      trainingStart_ = now;
      haptic_ = Haptic::Removed;
      hapticPressId_ = 0;
    }
    tick(now);
    return;
  }
  // Origin reports the debounced physical state through the separate sensor
  // observation lane. Only the server decides capture, cancellation and revival.
}

ButtonDecision GameModel::buttonPressed(uint32_t now, uint32_t pressId, bool trace) {
  if (profile_ == Profile::Training || academyPlayer()) {
    if (!trainingGhost_) return {ButtonDisposition::NotGhost, 0};
    trainingStart_ = now;
    haptic_ = Haptic::Found;
    hapticPressId_ = pressId;
    return {ButtonDisposition::LocalAccepted, 0};
  }
  if (!synchronized()) return {ButtonDisposition::Unsynchronized, 0};
  if (!activeGhost()) return {ButtonDisposition::NotGhost, 0};
  // 1 is the server's minimum count; writing it restarts the server's charge timer.
  if (emit(GameEvent::Kind::SetCount, 1, pressId, trace)) {
    queue_[(head_ + size_ - 1) % kCapacity].notBeforeMs = now + kFoundMs;
    count_ = 1;
    countPending_ = true;
    haptic_ = Haptic::Found;
    hapticPressId_ = pressId;
    return {ButtonDisposition::Queued, sequence_};
  }
  return {ButtonDisposition::QueueFull, 0};
}

void GameModel::tick(uint32_t now) {
  if (profile_ == Profile::Training || academyPlayer()) {
    if (trainingGhost_ && chipPresent_ && uint32_t(now - trainingStart_) >= 9000U)
      trainingGhost_ = false;
    count_ = trainingGhost_ ? uint8_t(1 + (uint32_t(now - trainingStart_) >= 9000U ? 3 :
        uint32_t(now - trainingStart_) / 3000U)) : 4;
    return;
  }
  // Origin: the server charges revival_count; the display follows its snapshots.
}

bool GameModel::canUseLifeDevice() const {
  return synchronized() && activeGhost() && server_.sacrificed && !server_.open && count_ == 4;
}

Feedback GameModel::feedback() {
  Feedback out;
  out.haptic = haptic_;
  out.hapticPressId = hapticPressId_;
  haptic_ = Haptic::None;
  hapticPressId_ = 0;
  if (profile_ == Profile::Training) {
    out.stateValid = true;  // Fixed default identity preserves explicit training events.
    out.display = trainingGhost_ ? Display::Ghost : Display::Player;
    out.lit = trainingGhost_ ? count_ : 4;
    return out;
  }
  out.stateValid = synchronized() && server_.valid;
  out.role = server_.role;
  out.deviceState = server_.deviceState;
  out.phase = server_.phase;
  out.stateEpoch = server_.connectionEpoch;
  if (!haveServer_) return out;
  if (server_.phase == Phase::Academy) {
    if (server_.deviceState == DeviceState::Player) {
      out.display = trainingGhost_ ? Display::Ghost : Display::Player;
      out.lit = trainingGhost_ ? count_ : 4;
    } else if (server_.deviceState == DeviceState::Tagger) {
      out.display = Display::Tagger;
      out.lit = 4;
      out.haptic = Haptic::None;
    }
    return out;
  }
  // Terminal state remains an explicit override. A synchronization loss keeps
  // rendering the last authoritative state instead of inventing a red warning.
  if (server_.phase == Phase::Ended || server_.deviceState == DeviceState::Ended) { out.display = Display::Ended; return out; }
  if (server_.phase == Phase::Photo || server_.deviceState == DeviceState::Photo) {
    // Photo mode exposes only tagger versus survivor. Ghost and neutral are
    // intentionally rendered as survivors so photo can never fall back to red.
    out.display = server_.role == Role::Tagger ? Display::Tagger : Display::Player;
    out.lit = 4;
    return out;
  }
  if (server_.phase == Phase::Unknown) return out;
  if (server_.deviceState == DeviceState::Setting || server_.deviceState == DeviceState::Ready) {
    out.display = server_.deviceState == DeviceState::Setting ? Display::Setting : Display::Ready;
    out.lit = chipPresent_ ? 4 : 3;
    return out;
  }
  // The selection animation is meaningful before game_state becomes activate.
  if (server_.role == Role::Tagger &&
      (server_.deviceState == DeviceState::Blink || server_.deviceState == DeviceState::Activate)) {
    out.display = server_.deviceState == DeviceState::Blink ? Display::TaggerBlink : Display::TaggerActive;
    return out;
  }
  if (server_.phase == Phase::Setting) return out;
  if (server_.phase == Phase::Ready) { out.display = Display::Ready; return out; }
  switch (server_.role) {
    case Role::Player:
      // Physical chip changes and unacknowledged requests do not change the role color.
      out.display = Display::Player;
      out.lit = 4;
      break;
    case Role::Ghost: out.display = Display::Ghost; out.lit = count_; break;
    case Role::Tagger: out.display = Display::Tagger; break;
    default: out.display = Display::Ready; break;
  }
  return out;
}

}  // namespace iotglove
