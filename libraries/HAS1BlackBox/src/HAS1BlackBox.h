#pragma once
#include "BlackBoxRecord.h"
namespace has1blackbox {
struct Config {
  const char *device;
  const char *firmware;
  const char *brokerUri;
  bool enabled = true;
  const char *username = "";
  const char *password = "";
  // Opt-in: accept a spiffs partition above 192 KiB (whole sectors, up to
  // 2 MiB), e.g. the default 4 MB scheme. Only its first 128 KiB is used.
  bool largePartition = false;
};
enum class FlushResult { Persisted, Failed, TimedOut, Unavailable };
struct Diagnostics {
  uint32_t accepted = 0, persisted = 0, dropped = 0, flashErrors = 0,
           uploadFailures = 0, uploaded = 0, queueDepth = 0;
  bool ready = false, paused = false;
  uint32_t eraseCalls = 0, maxEraseUs = 0, overwritten = 0;
  // spiffs partition bytes (0 = not found) and the last Ring::begin result as
  // RingStatus (0 unopened, 1 ready, 2 size, 3 read, 4 metadata, 5 foreign
  // data, 6 erase, 7 write, 8 record conflict).
  uint32_t partitionSize = 0;
  uint8_t ringStatus = 0;
  // Uploader task minimum free stack in bytes (0 = not measured yet).
  uint32_t uploaderStackFree = 0;
  // Saved coredump at boot: 0 not checked yet, 1 written by this boot's
  // panic/watchdog reset, 2 an older crash still stored, 3 none. Task name is
  // sanitized to [A-Za-z0-9_.-].
  uint8_t crashState = 0;
  uint32_t crashPc = 0, crashCause = 0;
  char crashTask[16] = {};
};
class BlackBox {
public:
  bool begin(const Config &);
  uint32_t log(EventType, int32_t a = 0, int32_t b = 0, int32_t c = 0,
               int32_t d = 0, const char *text = "");
  void setUploadAllowed(bool);
  void setPaused(bool);
  bool tryBeginLedOutput();
  void endLedOutput();
  bool ready() const;
  // No MQTT session exists. After setUploadAllowed(false) or setPaused(true)
  // this becomes true once the uploader has stopped and freed its client, and
  // stays true until permission returns. A leaked client is not waited for.
  bool uploadIdle() const;
  FlushResult flushThrough(uint32_t ticket, uint32_t timeoutMs);
  Diagnostics diagnostics() const;
};
extern BlackBox blackbox;
} // namespace has1blackbox
