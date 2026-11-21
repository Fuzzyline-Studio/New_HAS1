#include "HAS1BlackBox.h"
#if defined(ARDUINO_ARCH_ESP32)
#include "BlackBoxRing.h"
#include "BlackBoxUpload.h"
#include "BlackBoxEvidence.h"
#include "BlackBoxIdentity.h"
#include <esp_attr.h>
#include <Arduino.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <esp_core_dump.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mqtt_client.h>
#include <new>
namespace has1blackbox {
namespace {
struct Pending {
  Record record;
  uint32_t ticket;
  uint32_t wraps;
};
struct Control {
  bool ack;
  uint32_t token;
  uint32_t generation;
  uint64_t ring;
  uint32_t seq;
};
struct AckMessage {
  size_t size;
  char data[80];
};
std::atomic<bool> started{false}, isReady{false}, paused{false}, allowed{false};
std::atomic<uint32_t> generation{0}, accepted{0}, persisted{0}, dropped{0},
    flashErrors{0}, uploadFailures{0}, uploaded{0}, firstFailed{0},
    completed{0}, eraseCalls{0}, maxEraseUs{0}, overwritten{0},
    partitionBytes{0}, uploaderStackFree{0}, crashPc{0}, crashCause{0};
std::atomic<uint8_t> ringStatus{0}, crashState{0};
char crashTask[16]{}; // written once before crashState is published
uint32_t bootId = 0;
RTC_NOINIT_ATTR RetainedEvidence retainedEvidence;
ClockCoverage clockCoverage;
portMUX_TYPE clockMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<int> publishedMessage{-1};
char device[24]{}, firmware[28]{}, broker[192]{}, mac[13]{}, username[64]{},
    password[128]{};
QueueHandle_t events = nullptr, controls = nullptr, snapshots = nullptr,
              acks = nullptr;
SemaphoreHandle_t flashGuard = nullptr;
portMUX_TYPE producerMux = portMUX_INITIALIZER_UNLOCKED;
struct PartitionFlash : Flash {
  const esp_partition_t *p = nullptr;
  bool read(size_t o, uint8_t *b, size_t n) override {
    return p && esp_partition_read(p, o, b, n) == ESP_OK;
  }
  bool write(size_t o, const uint8_t *b, size_t n) override {
    return p && esp_partition_write(p, o, b, n) == ESP_OK;
  }
  bool erase(size_t o, size_t n) override {
    if (!p)
      return false;
    int64_t start = esp_timer_get_time();
    bool result = esp_partition_erase_range(p, o, n) == ESP_OK;
    uint32_t elapsed = uint32_t(esp_timer_get_time() - start);
    ++eraseCalls;
    uint32_t previous = maxEraseUs.load();
    while (previous < elapsed &&
           !maxEraseUs.compare_exchange_weak(previous, elapsed)) {
    }
    return result;
  }
  size_t size() const override { return p ? p->size : 0; }
};
PartitionFlash flash;
Ring ring(flash);
std::atomic<bool> connected{false}, subscribed{false}, mqttTaskStarted{false};
// An MQTT client (its task, buffers and TCP transport) may exist; see uploadIdle().
std::atomic<bool> sessionActive{false};
std::atomic<uint32_t> mqttStartTimeouts{0};
// Only MQTT task touches these reassembly fields; callbacks only copy to the
// ACK queue.
AckMessage assembling{};
size_t received = 0;
char ackTopic[80]{};
void mqttEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = static_cast<esp_mqtt_event_handle_t>(data);
  // Every event is dispatched by mqtt_task itself after it set client->run;
  // BEFORE_CONNECT is the first.
  mqttTaskStarted = true;
  if (id == MQTT_EVENT_BEFORE_CONNECT) return;
  if (id == MQTT_EVENT_PUBLISHED) { publishedMessage = e->msg_id; return; }
  if (id == MQTT_EVENT_CONNECTED) {
    connected = true;
    return;
  }
  if (id == MQTT_EVENT_DISCONNECTED) {
    connected = false;
    subscribed = false;
    received = 0;
    return;
  }
  if (id == MQTT_EVENT_SUBSCRIBED) {
    subscribed = true;
    return;
  }
  if (id != MQTT_EVENT_DATA)
    return;
  if (e->current_data_offset == 0) {
    received = 0;
    assembling.size = 0;
    if (e->retain || e->topic_len != int(std::strlen(ackTopic)) || !e->topic ||
        std::memcmp(e->topic, ackTopic, e->topic_len) ||
        e->total_data_len <= 0 ||
        e->total_data_len > int(sizeof(assembling.data)))
      return;
    assembling.size = size_t(e->total_data_len);
  }
  if (!assembling.size || e->current_data_offset != int(received) ||
      e->data_len <= 0 || received + size_t(e->data_len) > assembling.size) {
    assembling.size = 0;
    return;
  }
  std::memcpy(assembling.data + received, e->data, size_t(e->data_len));
  received += size_t(e->data_len);
  if (received == assembling.size) {
    xQueueSend(acks, &assembling, 0);
    assembling.size = 0;
    received = 0;
  }
}
bool networkReady() {
  if (paused || !allowed)
    return false;
  esp_netif_t *net = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip{};
  wifi_ap_record_t ap{};
  return net && esp_netif_get_ip_info(net, &ip) == ESP_OK && ip.ip.addr != 0 &&
         esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}
void markFailure(uint32_t ticket) {
  uint32_t zero = 0;
  firstFailed.compare_exchange_strong(zero, ticket);
}
// Image checksum of the last coredump already logged. Survives panic and
// software resets, not power loss: a kept image is re-logged at most once per
// power cycle (as older), and a new crash always differs.
constexpr uint32_t ReportedCoreMagic = 0x43524431; // "CRD1"
RTC_NOINIT_ATTR uint32_t reportedCore[2];
// One-shot task: summarize the coredump the panic handler saved in flash.
// Parsing runs on its own stack (the writer's has little headroom) and the
// image is kept for USB analysis. Records use the "prior." prefix: they
// describe a previous boot and are not copied into the RTC evidence slots.
void coreDumpTask(void *) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  esp_core_dump_summary_t summary{};
  for (;;) {
    while (paused || xSemaphoreTake(flashGuard, pdMS_TO_TICKS(5)) != pdTRUE)
      vTaskDelay(pdMS_TO_TICKS(5));
    if (!paused)
      break;
    xSemaphoreGive(flashGuard); // an OTA pause began after the check
  }
  size_t address = 0, size = 0;
  uint32_t checksum = 0;
  // The image ends with its CRC32, which differs for every crash instance.
  const bool found = esp_core_dump_image_check() == ESP_OK &&
                     esp_core_dump_get_summary(&summary) == ESP_OK &&
                     esp_core_dump_image_get(&address, &size) == ESP_OK && size >= 4 &&
                     esp_flash_read(nullptr, &checksum, uint32_t(address + size - 4), 4) == ESP_OK;
  xSemaphoreGive(flashGuard);
  if (!found) {
    crashState = 3;
  } else {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool seen = reportedCore[0] == ReportedCoreMagic && reportedCore[1] == checksum;
    // A task/interrupt watchdog panics with CONFIG_ESP_TASK_WDT_PANIC and so
    // writes a dump; the RTC watchdog (ESP_RST_WDT) never does.
    const bool fresh = !seen && (reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
                                 reason == ESP_RST_INT_WDT);
    size_t n = 0;
    for (; n + 1 < sizeof(crashTask) && summary.exc_task[n]; ++n) {
      const char c = summary.exc_task[n];
      const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
      crashTask[n] = plain ? c : '_';
    }
    crashTask[n] = 0;
    uint32_t cause = 0, vaddr = 0;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    cause = summary.ex_info.exc_cause;
    vaddr = summary.ex_info.exc_vaddr;
#endif
    crashPc = summary.exc_pc;
    crashCause = cause;
    crashState = fresh ? 1 : 2;
    if (!seen) { // not logged since the last power-on
      const uint32_t depth = summary.exc_bt_info.depth > 16 ? 16 : summary.exc_bt_info.depth;
      char text[28];
      std::snprintf(text, sizeof(text), "prior.core:%s", crashTask);
      // v[3]: backtrace depth | corrupted 0x100 | fresh 0x200 | reset reason << 16
      blackbox.log(EventType::Crash, int32_t(summary.exc_pc), int32_t(cause), int32_t(vaddr),
                   int32_t(depth | (summary.exc_bt_info.corrupted ? 0x100U : 0U) |
                           (fresh ? 0x200U : 0U) | (uint32_t(reason) << 16)),
                   text);
      for (uint32_t i = 0; i < depth; i += 4) {
        std::snprintf(text, sizeof(text), "prior.core.bt%lu", (unsigned long)(i / 4));
        const uint32_t *bt = summary.exc_bt_info.bt;
        blackbox.log(EventType::Crash, int32_t(bt[i]), int32_t(i + 1 < depth ? bt[i + 1] : 0),
                     int32_t(i + 2 < depth ? bt[i + 2] : 0),
                     int32_t(i + 3 < depth ? bt[i + 3] : 0), text);
      }
      // Matches esp_app_get_elf_sha256() of the build that crashed; v[1] is
      // the image checksum that identifies this crash instance.
      std::snprintf(text, sizeof(text), "prior.core.elf:%.9s",
                    reinterpret_cast<const char *>(summary.app_elf_sha256));
      blackbox.log(EventType::Crash, int32_t(summary.core_dump_version), int32_t(checksum), 0,
                   0, text);
      reportedCore[0] = ReportedCoreMagic;
      reportedCore[1] = checksum;
    }
  }
#else
  crashState = 3;
#endif
  vTaskDelete(nullptr);
}
void writer(void *) {
  flash.p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  // Startup only: partition validation and discovery happen off the game task.
  partitionBytes = flash.p ? uint32_t(flash.p->size) : 0;
  bool ok = flash.p && flash.p->size >= Ring::Size &&
            flash.p->size <= Ring::MaxSize;
  if (flash.p && !ok)
    ringStatus = uint8_t(RingStatus::BadSize);
  if (ok) {
    while (paused || xSemaphoreTake(flashGuard, pdMS_TO_TICKS(5)) != pdTRUE)
      vTaskDelay(pdMS_TO_TICKS(5));
    // A simultaneous pause may begin after this check; this is an in-flight
    // operation.
    if (paused) {
      xSemaphoreGive(flashGuard);
      ok = false;
    } else {
      ok = ring.begin((uint64_t(esp_random()) << 32) | esp_random());
      ringStatus = uint8_t(ring.status());
      xSemaphoreGive(flashGuard);
    }
  }
  isReady = ok;
  if (!ok)
    ++flashErrors;
  // After ring discovery so its records are persisted; failure only loses the
  // crash summary.
  xTaskCreate(coreDumpTask, "bb-core", 4096, nullptr, 1, nullptr);
  uint32_t retryAt = 0, lastDrop = 0, lastSummary = millis();
  Batch outstanding{};
  bool hasOutstanding = false, degraded = false;
  for (;;) {
    if (paused) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (degraded || !ring.ready()) {
      if (ok && uint32_t(millis() - retryAt) >= 60000) {
        retryAt = millis();
        if (xSemaphoreTake(flashGuard, 0) == pdTRUE) {
          isReady = ring.begin(0);
          ringStatus = uint8_t(ring.status());
          degraded = !isReady.load();
          xSemaphoreGive(flashGuard);
        }
      }
      if (degraded || !ring.ready()) {
        Pending p;
        if (xQueueReceive(events, &p, pdMS_TO_TICKS(5)) == pdTRUE) {
          markFailure(p.ticket);
          completed = p.ticket;
          ++dropped;
        }
        continue;
      }
    }
    // Every flash mutation uses a zero-wait guard; release between each
    // operation.
    Control control;
    if (xQueuePeek(controls, &control, 0) == pdTRUE &&
        xSemaphoreTake(flashGuard, 0) == pdTRUE) {
      if (!paused && xQueueReceive(controls, &control, 0) == pdTRUE) {
        if (control.ack) {
          if (hasOutstanding && control.ring == outstanding.ring &&
              control.token == outstanding.token &&
              control.seq == outstanding.records[outstanding.count - 1].seq()) {
            if (ring.acknowledge(control.seq)) {
              uploaded += outstanding.count;
              hasOutstanding = false;
            } else {
              ++flashErrors;
              isReady = false;
              degraded = true;
              retryAt = millis();
            }
          }
        } else if (control.generation == generation) {
          outstanding = {};
          outstanding.ring = ring.id();
          outstanding.token = control.token;
          outstanding.generation = control.generation;
          bool copied = ring.snapshot(outstanding.records, BatchRecords,
                                      outstanding.count);
          hasOutstanding = copied && outstanding.count > 0;
          if (!copied) {
            ++flashErrors;
            degraded = true;
            isReady = false;
            retryAt = millis();
            outstanding.count = 0;
          }
          xQueueOverwrite(snapshots, &outstanding);
        }
      }
      xSemaphoreGive(flashGuard);
    }
    Pending p;
    if (!degraded && xQueuePeek(events, &p, 0) == pdTRUE &&
        xSemaphoreTake(flashGuard, 0) == pdTRUE) {
      if (!paused && xQueueReceive(events, &p, 0) == pdTRUE) {
        AppendResult r = ring.append(p.record);
        if (r == AppendResult::Uncertain && ring.ready())
          r = ring.append(p.record); // fresh slot, never rewrite dirty slot
        if (r == AppendResult::Persisted) {
          ++persisted;
          portENTER_CRITICAL(&clockMux);
          clockCoverage.observeCurrentRecord(ring.id(),bootId,p.record,p.wraps);
          portEXIT_CRITICAL(&clockMux);
        } else {
          markFailure(p.ticket);
          ++dropped;
          if (r != AppendResult::Full) {
            ++flashErrors;
            degraded = true;
            retryAt = millis();
          }
        }
        completed = p.ticket;
        isReady = !degraded && ring.ready();
      }
      xSemaphoreGive(flashGuard);
    }
    // Queue losses are summarized without recursively calling the producer.
    uint32_t losses = dropped.load();
    if (!degraded && uint32_t(millis() - lastSummary) >= 60000 &&
        uxQueueMessagesWaiting(events) == 0 &&
        xSemaphoreTake(flashGuard, 0) == pdTRUE) {
      lastSummary = millis();
      if (!paused) {
        Record r = Record::make(EventType::Dropped, bootId, millis(),
                                int32_t(losses), int32_t(losses - lastDrop));
        bool summaryOk = ring.checkpointAck();
        if (summaryOk && losses != lastDrop) {
          summaryOk = ring.append(r) == AppendResult::Persisted;
          if (summaryOk)
            lastDrop = losses;
        }
        // A 64-bit monotonic anchor makes millis() rollover explicit within
        // each boot.
        uint64_t up = uint64_t(esp_timer_get_time()) / 1000;
        Record anchor = Record::make(
            EventType::Logger, bootId, uint32_t(up), int32_t(up >> 32),
            int32_t(flashErrors.load()), int32_t(uploadFailures.load()),
            int32_t(losses), "uptime64/errors");
        if (summaryOk)
          summaryOk = ring.append(anchor) == AppendResult::Persisted;
        Record flashStats = Record::make(
            EventType::Logger, bootId, millis(), int32_t(eraseCalls.load()),
            int32_t(maxEraseUs.load()), int32_t(ring.lostRecords()),
            int32_t(uxTaskGetStackHighWaterMark(nullptr)), "erase/lost/stack");
        if (summaryOk)
          summaryOk = ring.append(flashStats) == AppendResult::Persisted;
        // Flash-only builds create no uploader; skip the all-zero record.
        if (summaryOk && broker[0]) {
          Record uploadStats = Record::make(
              EventType::Logger, bootId, millis(), int32_t(uploaderStackFree.load()),
              int32_t(uploaded.load()), int32_t(uploadFailures.load()),
              int32_t(mqttStartTimeouts.load()), "upload/stack");
          summaryOk = ring.append(uploadStats) == AppendResult::Persisted;
        }
        if (!summaryOk) {
          ++flashErrors;
          degraded = true;
          isReady = false;
          retryAt = millis();
        }
      }
      xSemaphoreGive(flashGuard);
    }
    overwritten = ring.lostRecords();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
void uploader(void *) {
  // Off this stack, and only allocated on builds that upload at all.
  char *uploadPayload = static_cast<char *>(std::malloc(PayloadCapacity));
  if (!uploadPayload) {
    ++uploadFailures;
    vTaskDelete(nullptr);
  }
  char topic[80];
  std::snprintf(topic, sizeof(topic), "has1/blackbox/v1/%s/batch", mac);
  std::snprintf(ackTopic, sizeof(ackTopic), "has1/blackbox/v1/%s/ack", mac);
  char clientId[32];
  std::snprintf(clientId, sizeof(clientId), "has1-bb-%s", mac);
  unsigned failures = 0;
  uint32_t nextAt = millis();
  for (;;) {
    if (int32_t(millis() - nextAt) < 0 || !isReady || !networkReady()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    uint32_t gen = generation.load(), token = esp_random(), start = millis();
    Batch batch{};
    Control request{false, token, gen, 0, 0};
    bool success = false, attempted = false, cancelled = false;
    xQueueReset(snapshots);
    xQueueReset(acks);
    if (xQueueSend(controls, &request, 0) != pdTRUE) {
      nextAt = millis() + 5000;
      continue;
    }
    while (uint32_t(millis() - start) < 1500 && generation == gen &&
           networkReady()) {
      if (xQueueReceive(snapshots, &batch, pdMS_TO_TICKS(5)) == pdTRUE &&
          batch.token == token)
        break;
    }
    if (!batch.count || generation != gen || !networkReady()) {
      nextAt = millis() + 5000;
      continue;
    }
    size_t bytes = encodeBatch(batch, device, mac, uploadPayload, PayloadCapacity);
    if (!bytes) {
      ++uploadFailures;
      nextAt = millis() + 60000;
      continue;
    }
    // Published before the last permission check: a caller that withdrew
    // permission and then saw uploadIdle() knows no session can still start.
    sessionActive = true;
    if (generation != gen || !networkReady()) {
      sessionActive = false;
      nextAt = millis() + 5000;
      continue;
    }
    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri = broker;
    cfg.credentials.client_id = clientId;
    cfg.credentials.username = username[0] ? username : nullptr;
    cfg.credentials.authentication.password = password[0] ? password : nullptr;
    cfg.network.timeout_ms = 250;
    cfg.network.disable_auto_reconnect = true;
    // stop() after a failed connect waits out half of this in WAIT_RECONNECT.
    cfg.network.reconnect_timeout_ms = 100;
    cfg.task.priority = 1;
    cfg.task.stack_size = 4096;
    cfg.buffer.size = 512;
    cfg.buffer.out_size = 512;
    cfg.outbox.limit = 4096;
    cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    cfg.session.keepalive = 15;
    connected = false;
    subscribed = false;
    mqttTaskStarted = false;
    assembling.size = 0;
    received = 0;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (client) {
      // Without the handler no event would confirm the task start; never
      // start one then.
      attempted = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqttEvent,
                                                 nullptr) == ESP_OK &&
                  esp_mqtt_client_start(client) == ESP_OK;
      bool subscribedRequest = false, sent = false, probed = false;
      int probeMessage = -1;
      uint32_t acknowledgedAt = 0;
      publishedMessage = -1;
      while (attempted && uint32_t(millis() - start) < 1500) {
        if (generation != gen || !networkReady()) {
          cancelled = true;
          break;
        }
        if (connected && !subscribedRequest) {
          subscribedRequest =
              esp_mqtt_client_subscribe(client, ackTopic, 1) >= 0;
        }
        if (subscribed && !sent) {
          // Written by this task now. An enqueued message waits for the MQTT
          // task's next read poll (up to ~1 s), and a game poll in between
          // cancels the session and discards it before it is ever sent.
          sent = esp_mqtt_client_publish(client, topic, uploadPayload, int(bytes), 1,
                                         0) >= 0;
          if (!sent)
            break;
        }
        AckMessage ack;
        if (sent && xQueueReceive(acks, &ack, 0) == pdTRUE) {
          char challenge[33];
          if(!probed && parseTimeProbe(ack.data,ack.size,challenge)) {
            probed=true;
            ClockCoverage coverage;
            portENTER_CRITICAL(&clockMux);
            coverage=clockCoverage;
            portEXIT_CRITICAL(&clockMux);
            const uint64_t up=uint64_t(esp_timer_get_time())/1000;
            char response[320];
            const size_t n=encodeTimeProbe(coverage,mac,challenge,up,response,sizeof(response));
            if(n) probeMessage=esp_mqtt_client_publish(client,topic,response,int(n),1,0);
          } else if(!success && exactAck(ack.data,ack.size,batch)) {
            Control save{true,token,gen,batch.ring,batch.records[batch.count-1].seq()};
            success=xQueueSend(controls,&save,0)==pdTRUE;
            acknowledgedAt=millis();
            if(!success) break;
          }
        }
        // Only the diagnostic task waits, within the original session target.
        // Normal ACK persistence never depends on the optional probe response.
        if(success && (probeMessage<0 || publishedMessage==probeMessage ||
                       uint32_t(millis()-acknowledgedAt)>=50)) break;
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      // esp_mqtt_client_start() only creates mqtt_task, which sets client->run
      // once it runs. A session cancelled before that made stop() return at
      // once and destroy() free the client under the starting task: field
      // panics, LoadProhibited at esp_mqtt_task/run_event_loop with a NULL
      // client->config. Wait for the task's first event; then stop() waits for
      // it to finish.
      if (attempted) {
        const uint32_t waitFrom = millis();
        while (!mqttTaskStarted && uint32_t(millis() - waitFrom) < 10000)
          vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (attempted && !mqttTaskStarted) {
        // Either starved or exited early (transport allocation failure); it
        // cannot be told apart, so never free a client a task may still use.
        // Leaking is bounded: the uploader stops after three.
        if (++mqttStartTimeouts >= 3) {
          ++uploadFailures;
          sessionActive = false;
          for (;;)
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
      } else {
        // SDK stop/destroy can exceed the session target. Only this task
        // waits; neither device task nor OTA ever waits for it. Stop discards
        // bounded RAM outbox.
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
      }
    }
    sessionActive = false;
    // Lifetime minimum: covers the payload buffer and publish/subscribe paths.
    uploaderStackFree = uint32_t(uxTaskGetStackHighWaterMark(nullptr));
    if (success) {
      failures = 0;
      nextAt = millis() + 5000;
    } else if (cancelled) {
      nextAt = millis() + 5000;
    } else {
      ++uploadFailures;
      uint32_t delay = backoffMs(failures);
      if (failures < 4)
        ++failures;
      nextAt =
          millis() + delay - delay / 5 + esp_random() % (delay * 2 / 5 + 1);
    }
  }
}
} // namespace
BlackBox blackbox;
bool BlackBox::begin(const Config &c) {
  if (!c.enabled)
    return false;
  if (started)
    return false;
  if (!c.device || (std::strcmp(c.device, "iotglove") &&
                    std::strcmp(c.device, "HAS1_generator") &&
                    std::strcmp(c.device, "HAS1_altar")))
    return false;
  if (c.brokerUri && std::strlen(c.brokerUri) >= sizeof(broker))
    return false;
  if ((c.username && std::strlen(c.username) >= sizeof(username)) ||
      (c.password && std::strlen(c.password) >= sizeof(password)))
    return false;
  std::snprintf(password, sizeof(password), "%s", c.password ? c.password : "");
  std::snprintf(device, sizeof(device), "%s", c.device);
  std::snprintf(firmware, sizeof(firmware), "%s",
                c.firmware ? c.firmware : "unknown");
  std::snprintf(broker, sizeof(broker), "%s", c.brokerUri ? c.brokerUri : "");
  // Only IPv4 literals on a trusted local network; avoid unbounded DNS
  // resolution.
  if (broker[0]) {
    unsigned a, b, c1, d, port;
    int end = 0;
    if (std::sscanf(broker, "mqtt://%u.%u.%u.%u:%u%n", &a, &b, &c1, &d, &port,
                    &end) != 5 ||
        broker[end] || a > 255 || b > 255 || c1 > 255 || d > 255 || port == 0 ||
        port > 65535)
      return false;
  }
  uint8_t address[6];
  if (esp_read_mac(address, ESP_MAC_WIFI_STA) != ESP_OK)
    return false;
  std::snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X", address[0],
                address[1], address[2], address[3], address[4], address[5]);
  std::snprintf(username, sizeof(username), "%s",
                resolveMqttUsername(c.username, broker, mac));
  bootId = esp_random();
  events = xQueueCreate(32, sizeof(Pending));
  controls = xQueueCreate(8, sizeof(Control));
  snapshots = xQueueCreate(1, sizeof(Batch));
  acks = xQueueCreate(8, sizeof(AckMessage));
  flashGuard = xSemaphoreCreateMutex();
  if (!events || !controls || !snapshots || !acks || !flashGuard) {
    if (events)
      vQueueDelete(events);
    if (controls)
      vQueueDelete(controls);
    if (snapshots)
      vQueueDelete(snapshots);
    if (acks)
      vQueueDelete(acks);
    if (flashGuard)
      vSemaphoreDelete(flashGuard);
    events = controls = snapshots = acks = nullptr;
    flashGuard = nullptr;
    return false;
  }
  started = true;
  if (xTaskCreate(writer, "bb-writer", 4096, nullptr, 1, nullptr) != pdPASS) {
    started = false;
    vQueueDelete(events);
    vQueueDelete(controls);
    vQueueDelete(snapshots);
    vQueueDelete(acks);
    vSemaphoreDelete(flashGuard);
    events = controls = snapshots = acks = nullptr;
    flashGuard = nullptr;
    return false;
  }
  if (broker[0] &&
      // 8 KiB with the 4 KiB payload on the stack left 200-360 bytes in the
      // field; with it on the heap, 6 KiB leaves about 2 KiB.
      xTaskCreate(uploader, "bb-upload", 6144, nullptr, 1, nullptr) != pdPASS)
    ++uploadFailures;
  const auto reset=esp_reset_reason();
  const bool warm=reset==ESP_RST_SW || reset==ESP_RST_PANIC || reset==ESP_RST_INT_WDT ||
      reset==ESP_RST_TASK_WDT || reset==ESP_RST_WDT || reset==ESP_RST_DEEPSLEEP;
  Record prior[RetainedEvidence::Capacity];
  const size_t priorCount=retainedEvidence.recover(prior,warm);
  retainedEvidence.clear();
  // Replay retains original boot and uptime; these are not current-boot events.
  for(size_t i=0;i<priorCount;++i) {
    Pending p{prior[i],uint32_t(accepted.load()+1),0};
    if(xQueueSend(events,&p,0)==pdTRUE) accepted=p.ticket; else ++dropped;
  }
  log(EventType::Logger,int32_t(priorCount),warm,1,0,"rtc.replay.end");
  log(EventType::Boot,int32_t(reset),2,int32_t(priorCount),warm,"evidence.20261024.2");
  log(EventType::Boot,int32_t(reset),Unknown,Unknown,Unknown,firmware);
  return true;
}
uint32_t BlackBox::log(EventType type, int32_t a, int32_t b, int32_t c,
                       int32_t d, const char *text) {
  if (!started)
    return 0;
  const uint64_t up=uint64_t(esp_timer_get_time())/1000;
  Pending p{Record::make(type,bootId,uint32_t(up),a,b,c,d,text),0,uint32_t(up>>32)};
  uint32_t result = 0;
  portENTER_CRITICAL(&producerMux);
  // Fixed eight-slot RTC copy; no flash, socket, allocation or recovery wait.
  const bool replay=text && !std::strncmp(text,"prior.",6);
  const bool routine=text && (!std::strcmp(text,"beetle.online") ||
      !std::strcmp(text,"recovery.worker_stack"));
  if(!replay && !routine && (type==EventType::Crash || type==EventType::Recovery ||
      type==EventType::Peer || type==EventType::WifiEvent ||
      (type==EventType::HttpSummary && text &&
       (!std::strncmp(text,"http.",5) || std::strstr(text,"failure")))))
    retainedEvidence.add(p.record);
  uint32_t previous = accepted.load();
  if (previous != UINT32_MAX) {
    p.ticket = previous + 1;
    if (xQueueSend(events, &p, 0) == pdTRUE) {
      accepted = p.ticket;
      result = p.ticket;
    }
  }
  if (!result)
    ++dropped;
  portEXIT_CRITICAL(&producerMux);
  return result;
}
void BlackBox::setUploadAllowed(bool value) {
  bool old = allowed.exchange(value);
  if (old && !value)
    ++generation;
}
void BlackBox::setPaused(bool value) {
  paused = value;
  ++generation;
}
bool BlackBox::tryBeginLedOutput() {
  return !started || xSemaphoreTake(flashGuard, 0) == pdTRUE;
}
void BlackBox::endLedOutput() {
  if (started)
    xSemaphoreGive(flashGuard);
}
bool BlackBox::ready() const { return isReady; }
bool BlackBox::uploadIdle() const { return !sessionActive; }
Diagnostics BlackBox::diagnostics() const {
  Diagnostics d{accepted.load(),
          persisted.load(),
          dropped.load(),
          flashErrors.load(),
          uploadFailures.load(),
          uploaded.load(),
          events ? uint32_t(uxQueueMessagesWaiting(events)) : 0,
          isReady.load(),
          paused.load(),
          eraseCalls.load(),
          maxEraseUs.load(),
          overwritten.load(),
          partitionBytes.load(),
          ringStatus.load()};
  d.uploaderStackFree = uploaderStackFree.load();
  d.crashState = crashState.load();
  if (d.crashState) {
    d.crashPc = crashPc.load();
    d.crashCause = crashCause.load();
    std::memcpy(d.crashTask, crashTask, sizeof(d.crashTask));
  }
  return d;
}
FlushResult BlackBox::flushThrough(uint32_t ticket, uint32_t timeoutMs) {
  if (!started || !ticket || ticket > accepted)
    return FlushResult::Unavailable;
  timeoutMs = timeoutMs > 100 ? 100 : timeoutMs;
  uint32_t start = millis();
  for (;;) {
    uint32_t failure = firstFailed.load();
    if (failure && failure <= ticket)
      return FlushResult::Failed;
    if (completed >= ticket)
      return FlushResult::Persisted;
    if (uint32_t(millis() - start) >= timeoutMs)
      return FlushResult::TimedOut;
    vTaskDelay(1);
  }
}
} // namespace has1blackbox
#endif
