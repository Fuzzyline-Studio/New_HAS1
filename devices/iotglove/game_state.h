#pragma once

#include <stddef.h>
#include <stdint.h>

namespace iotglove {

// Raw 0..255 NeoPixel brightness applied before the server is heard from, and
// whenever its brightness field is missing or out of range. Same value as the
// other HAS1 devices' DEFAULT_BRIGHTNESS.
constexpr uint8_t kDefaultBrightness = 50;

enum class Profile : uint8_t { Origin, Training };
enum class Phase : uint8_t { Unknown, Setting, Ready, Photo, Academy, Active, Ended };
enum class Role : uint8_t { Neutral, Player, Tagger, Ghost };
enum class DeviceState : uint8_t { Other, Setting, Ready, Blink, Activate, Photo, Player, Tagger, Ended };
enum class Display : uint8_t { Setting, Ready, Player, Ghost, Tagger, TaggerActive, TaggerBlink, Ended };
enum class Haptic : uint8_t { None, Removed, Found };

// Owned by the network task, copied through a queue. No String/JSON crosses tasks.
struct ServerSnapshot {
  bool valid = false;
  uint32_t receivedAtMs = 0;
  char session[40] = {};
  char deviceName[24] = {};
  Phase phase = Phase::Unknown;
  Role role = Role::Neutral;
  DeviceState deviceState = DeviceState::Other;
  // Unlike mutation sessions, this epoch does not change for a game phase change.
  uint32_t connectionEpoch = 0;
  uint8_t revivalCount = 0;
  uint32_t stepSeconds = 0;
  bool sacrificed = false;
  bool open = false;
  uint8_t vibe = 0;
  int32_t lifeChip = 0;
  // Raw 0..255, already converted by the network task at parse time.
  uint8_t brightness = kDefaultBrightness;
  bool updateRequested = false;
  // Complete command identity is retained so github@12:7 -> github@13:7 is a
  // new request even when updateRequested remains true. Zero targets = latest.
  char updateCommand[40] = {};
  uint32_t otaTtgoVersion = 0;
  uint32_t otaBeetleVersion = 0;
  bool resetRequested = false;
};

// Observation only: decisions are returned by the same branch that acts on input.
enum class ButtonDisposition : uint8_t {
  Release = 0, LocalAccepted = 1, Queued = 2, NotGhost = 3, Unsynchronized = 4, QueueFull = 5
};
struct ButtonDecision {
  ButtonDisposition disposition = ButtonDisposition::NotGhost;
  uint32_t sequence = 0;
};

struct GameEvent {
  enum class Kind : uint8_t { SetCount };
  Kind kind = Kind::SetCount;
  uint32_t sequence = 0;
  uint32_t pressId = 0;  // Boot-local diagnostic identity; never sent to the game server.
  bool traceButton = false;
  char session[40] = {};
  char deviceName[24] = {};
  uint8_t value = 0;
  uint32_t notBeforeMs = 0;  // Not submitted to the network worker before this millis().
};

struct Feedback {
  Display display = Display::Setting;
  uint8_t lit = 4;
  Haptic haptic = Haptic::None;
  uint32_t hapticPressId = 0;
  // Semantic identity excludes physical chip and progress changes.
  Role role = Role::Neutral;
  DeviceState deviceState = DeviceState::Other;
  Phase phase = Phase::Unknown;
  bool stateValid = false;
  uint32_t stateEpoch = 0;
};

// Debounces a physical level; update() returns exactly one event per stable edge.
class DebouncedInput {
 public:
  explicit DebouncedInput(uint32_t intervalMs = 30) : intervalMs_(intervalMs) {}
  void begin(bool level, uint32_t now);
  bool update(bool level, uint32_t now);
  bool value() const { return stable_; }
 private:
  uint32_t intervalMs_;
  uint32_t candidateSince_ = 0;
  bool stable_ = false;
  bool candidate_ = false;
};

class GameModel {
 public:
  explicit GameModel(Profile profile) : profile_(profile) {}
  void begin(bool chipPresent, uint32_t now);
  void applyServer(const ServerSnapshot& snapshot, uint32_t now);
  void chipChanged(bool present, uint32_t now);
  ButtonDecision buttonPressed(uint32_t now, uint32_t pressId = 0, bool trace = false);
  void tick(uint32_t now);
  bool peekEvent(GameEvent& out) const;
  void consumeEvent();
  // A command with uncertain outcome must not be blindly regenerated.
  void commandUncertain(uint32_t sequence);
  Feedback feedback();
  bool chipPresent() const { return chipPresent_; }
  bool synchronized() const { return haveServer_ && !needsSync_; }
  bool queueOverflowed() const { return overflow_; }
  size_t queuedEvents() const { return size_; }
  bool canUseLifeDevice() const;
  uint8_t count() const { return count_; }
  const ServerSnapshot& server() const { return server_; }
  bool academy() const { return haveServer_ && server_.phase == Phase::Academy; }
  bool academyTagger() const {
    return academy() && server_.deviceState == DeviceState::Tagger;
  }
 private:
  static constexpr size_t kCapacity = 12;
  Profile profile_;
  ServerSnapshot server_;
  GameEvent queue_[kCapacity];
  size_t head_ = 0;
  size_t size_ = 0;
  uint32_t sequence_ = 0;
  uint32_t trainingStart_ = 0;
  uint8_t count_ = 0;
  uint8_t observedCount_ = 0;
  bool chipPresent_ = false;
  bool trainingGhost_ = false;
  bool haveServer_ = false;
  bool needsSync_ = false;
  bool overflow_ = false;
  bool countPending_ = false;
  Haptic haptic_ = Haptic::None;
  uint32_t hapticPressId_ = 0;
  bool emit(GameEvent::Kind kind, uint8_t value = 0, uint32_t pressId = 0, bool trace = false);
  void resetQueue();
  bool activeGhost() const;
  bool academyPlayer() const;
};

}  // namespace iotglove
