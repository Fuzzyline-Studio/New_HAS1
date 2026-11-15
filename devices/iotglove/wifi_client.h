#pragma once
#include "game_state.h"
#include "network_recovery.h"

namespace iotglove {
struct GameResult {
  enum class Status : uint8_t { Success, Unknown, Rejected };
  uint32_t sequence = 0;
  Status status = Status::Unknown;
};
enum class NetworkOtaStatus : uint8_t { Idle, Queued, Running, Skipped, Failed };

// Single worker owns HAS2_Wifi, JSON, HTTP and OTA; all loop-facing calls are nonblocking.
bool networkBegin(int firmwareVersion, int partitionVersion);
// Coherent, nonblocking cross-core copy; contains no credentials or payload.
bool networkDiagnostics(NetworkDiagnostics& diagnostics);
bool networkPoll(ServerSnapshot& snapshot);
bool networkPollResult(GameResult& result);
bool networkSubmit(const GameEvent& event);
void networkRequestSnapshot();
// Hold new worker requests until untilMs (bounded by kNetworkQuietMaxMs).
void networkQuietUntil(uint32_t untilMs);
// Latest debounced physical state; absolute reporting does not require a game phase.
void networkReportChip(bool present);
void networkReportLocation(const char* room);
void networkReportBattery(float volts);
// Call only after matching Beetle OTA updated/skipped result, outside a live game.
bool networkRequestOta(uint32_t targetVersion = 0, const char* sourceCommand = "");
NetworkOtaStatus networkOtaStatus();
void networkClearOtaStatus();
}  // namespace iotglove
