#include "HAS1_altar.h"
#include <BlackBoxWifi.h>
#include <esp_heap_caps.h>

// 제단 블랙박스: 태그/칩/크랭크/집계 요청/솔레노이드 흐름을 flash 링에 남기고,
// 게임(game_state=activate) 밖에서만 MQTT로 수집 PC에 올린다. 게임 HTTP, 워치독,
// OTA, 파티션은 바꾸지 않는다. 기록 형식과 한계는 BLACKBOX.md 참고.

// 게임 중에는 올리지 않는다. 서버 상태를 한 번도 못 받았으면(Unknown) 닫아 둔다.
// GitHub OTA는 블랙박스를 켜지 않는 별도 부팅에서 돈다(game_state.ino).
static void BlackBoxUploadGate()
{
  const char *phase = (const char *)my["game_state"];
  const bool known = phase && phase[0];
  has1blackbox::blackbox.setUploadAllowed(
      known && strcmp(phase, "activate") != 0 && WiFi.status() == WL_CONNECTED);
}

void BlackBoxBegin()
{
  static char version[16];
  snprintf(version, sizeof(version), "%d", FIRMWARE_VER);
  has1blackbox::blackbox.begin({"HAS1_altar", version, HAS1_ALTAR_BLACKBOX_BROKER_URI,
                                HAS1_ALTAR_BLACKBOX_ENABLED != 0, HAS1_ALTAR_BLACKBOX_MQTT_USERNAME,
                                HAS1_ALTAR_BLACKBOX_MQTT_PASSWORD});
  has1blackbox::blackbox.log(has1blackbox::EventType::Boot, esp_reset_reason(), FIRMWARE_VER,
                             PARTITION_VER, 0, "device.boot");
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    has1blackbox::logWifiMetadata(event, info);
  });
}

void BlackBoxLoop()
{
  NeoRetryPending();

  static uint32_t previous = 0, emitted = 0, gate_at = 0, max_gap = 0, loops = 0;
  static bool seen = false;
  const uint32_t now = millis();
  if (seen)
  {
    const uint32_t gap = now - previous;
    if (gap > max_gap) max_gap = gap;
  }
  else
  {
    seen = true;
    emitted = now;
    gate_at = now - 250;
  }
  previous = now;
  ++loops;

  if (uint32_t(now - gate_at) >= 250)
  {
    gate_at = now;
    BlackBoxUploadGate();
  }
  if (uint32_t(now - emitted) < 60000) return;

  // 60초 요약: loop가 가장 오래 멈춘 시간(HTTP/통전 포함)과 메모리, 와이파이.
  has1blackbox::blackbox.log(has1blackbox::EventType::Progress, max_gap, loops, 0, 0, "main.loop");
  has1blackbox::blackbox.log(has1blackbox::EventType::Health, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                             uxTaskGetStackHighWaterMark(nullptr), "memory.main");
  has1blackbox::blackbox.log(has1blackbox::EventType::WifiSummary, int(WiFi.status()),
                             WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : INT32_MIN, 0, 0, "wifi");
  emitted = now;
  max_gap = 0;
  loops = 0;
}

// Recovery 유형으로 남긴다: 라이브러리가 이 유형의 최근 8건을 RTC에도 복사해 두므로,
// 패닉/워치독 재부팅 직전의 흐름이 flash에 못 내려갔어도 다음 부팅에서 다시 기록된다.
void BlackBoxAltar(const char *label, int32_t a, int32_t b, int32_t c, int32_t d)
{
  has1blackbox::blackbox.log(has1blackbox::EventType::Recovery, a, b, c, d, label);
}

void BlackBoxAltarState(int32_t kind, const char *state)
{
  has1blackbox::blackbox.log(has1blackbox::EventType::State, kind, 0, 0, 0,
                             state && state[0] ? state : "unknown");
  // 게임이 시작되면 다음 250ms 주기를 기다리지 않고 바로 업로드를 닫는다.
  if (kind == 0) BlackBoxUploadGate();
}

int BlackBoxDiagnosticsText(char *out, size_t cap)
{
  const has1blackbox::Diagnostics d = has1blackbox::blackbox.diagnostics();
  return snprintf(out, cap,
                  " heap=%lu heap_block=%lu ota_boot=%u"
                  " bb_ready=%u bb_status=%u bb_partition=%lu bb_flash_errors=%lu bb_uploaded=%lu"
                  " bb_upload_failures=%lu bb_dropped=%lu bb_core_state=%u bb_core_pc=0x%08lx",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  OtaLeanBoot() ? 1u : 0u,
                  d.ready ? 1u : 0u, static_cast<unsigned>(d.ringStatus),
                  static_cast<unsigned long>(d.partitionSize),
                  static_cast<unsigned long>(d.flashErrors), static_cast<unsigned long>(d.uploaded),
                  static_cast<unsigned long>(d.uploadFailures), static_cast<unsigned long>(d.dropped),
                  static_cast<unsigned>(d.crashState), static_cast<unsigned long>(d.crashPc));
}

//**************************************** Neopixel output guard ****************************************
// flash 쓰기는 캐시를 잠깐 꺼서 진행 중인 LED 전송을 끊을 수 있다. 블랙박스 writer와
// 같은 가드를 대기 없이 잡고 show()를 보낸다. 못 잡으면 스트립별 보류 표시만 남기고,
// 다음 loop의 NeoRetryPending()이 그 시점의 픽셀 버퍼를 보낸다(loop는 기다리지 않는다).
static Adafruit_NeoPixel *const neo_strips[] = {
    &pixels_square, &pixels_round, &pixels_side, &pixels_square2, &pixels_pn532};
static const size_t NEO_STRIP_COUNT = sizeof(neo_strips) / sizeof(neo_strips[0]);
static bool neo_pending[NEO_STRIP_COUNT] = {};

void NeoShow(Adafruit_NeoPixel &pixels)
{
  size_t index = NEO_STRIP_COUNT;
  for (size_t i = 0; i < NEO_STRIP_COUNT; ++i)
  {
    if (neo_strips[i] == &pixels) index = i;
  }
  if (!has1blackbox::blackbox.tryBeginLedOutput())
  {
    if (index < NEO_STRIP_COUNT) neo_pending[index] = true;
    return;
  }
  if (index < NEO_STRIP_COUNT) neo_pending[index] = false;
  pixels.show();
  has1blackbox::blackbox.endLedOutput();
}

void NeoRetryPending()
{
  for (size_t i = 0; i < NEO_STRIP_COUNT; ++i)
  {
    if (neo_pending[i]) NeoShow(*neo_strips[i]);
  }
}

// 통전 직전 암전처럼 "지금 반드시 나가야 하는" 화면용. waitMs 동안 가드를 기다리고,
// 그래도 못 잡은 스트립은 가드 없이 보낸다(드문 LED 깨짐보다 암전 누락이 더 나쁘다).
void NeoFlushPending(uint32_t waitMs)
{
  const uint32_t started = millis();
  for (;;)
  {
    NeoRetryPending();
    bool any = false;
    for (size_t i = 0; i < NEO_STRIP_COUNT; ++i) any = any || neo_pending[i];
    if (!any) return;
    if (uint32_t(millis() - started) >= waitMs) break;
    delay(1);
  }
  int32_t forced = 0;
  for (size_t i = 0; i < NEO_STRIP_COUNT; ++i)
  {
    if (!neo_pending[i]) continue;
    neo_pending[i] = false;
    neo_strips[i]->show();
    ++forced;
  }
  // 가드 없이 보낸 프레임은 깨졌을 수 있다 - 일어났다는 사실을 남긴다.
  BlackBoxAltar("altar.led.forced", forced, (int32_t)waitMs, 0, 0);
}
