#include "HAS1_altar.h"
#include <esp_bt.h>

// Arduino ESP32 코어는 부팅(initArduino) 시 btInUse()가 false면 BT 컨트롤러
// 메모리(~36KB)를 반납해버린다. 이 스케치는 BLE 라이브러리를 include 하지 않고
// esp_bt_* 를 직접 호출하므로, 기본 weak btInUse()(false)가 적용되면 컨트롤러
// 메모리가 사라져 btStart()/컨트롤러 init 이 항상 실패한다.
// 아래 strong 정의로 덮어써서 BT 메모리를 유지시킨다. (extern "C" 필수)
extern "C" bool btInUse(void)
{
  return true;
}

#define HAS3_ADV_INTERVAL_UNITS 400
#define HAS3_ADV_RETRY_MS 2000
#define HAS3_ADV_DATA_MAX 31
#define HCI_PACKET_COMMAND 0x01
#define HCI_PACKET_EVENT 0x04
#define HCI_EVENT_COMMAND_COMPLETE 0x0E
#define HCI_EVENT_COMMAND_STATUS 0x0F
#define HCI_OPCODE_RESET 0x0C03
#define HCI_OPCODE_LE_SET_ADV_PARAMS 0x2006
#define HCI_OPCODE_LE_SET_ADV_DATA 0x2008
#define HCI_OPCODE_LE_SET_SCAN_RESPONSE_DATA 0x2009
#define HCI_OPCODE_LE_SET_ADV_ENABLE 0x200A
#define HAS3_ADV_DEBUG 1
#define HAS3_LOCAL_NAME_MAX_LEN (5 + HAS3_MAX_DEVICE_NAME_LEN)

#if HAS3_ADV_DEBUG
#define ADV_LOG(msg) Serial.println(F(msg))
#define ADV_LOG_VALUE(prefix, value) \
  do                                 \
  {                                  \
    Serial.print(F(prefix));         \
    Serial.println(value);           \
  } while (0)
#else
#define ADV_LOG(msg) \
  do                 \
  {                  \
  } while (0)
#define ADV_LOG_VALUE(prefix, value) \
  do                                 \
  {                                  \
  } while (0)
#endif

static bool ble_adv_initialized = false;
static bool ble_adv_running = false;
static bool ble_adv_retry_pending = false;
static unsigned long ble_adv_last_retry_ms = 0;
static char ble_adv_device_name[HAS3_MAX_DEVICE_NAME_LEN + 1] = "";
static char ble_adv_local_name[HAS3_LOCAL_NAME_MAX_LEN + 1] = "";
static volatile bool hci_command_done = false;
static volatile uint16_t hci_command_opcode = 0;
static volatile uint8_t hci_command_status = 0xFF;

static void VhciSendAvailable()
{
}

static int VhciReceive(uint8_t *data, uint16_t len)
{
  if (data == nullptr || len < 7 || data[0] != HCI_PACKET_EVENT)
  {
    return 0;
  }

  if (data[1] == HCI_EVENT_COMMAND_COMPLETE)
  {
    hci_command_opcode = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    hci_command_status = data[6];
    hci_command_done = true;
  }
  else if (data[1] == HCI_EVENT_COMMAND_STATUS)
  {
    hci_command_status = data[3];
    hci_command_opcode = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
    hci_command_done = true;
  }
  return 0;
}

static esp_vhci_host_callback_t vhci_callbacks = {
    VhciSendAvailable,
    VhciReceive};

static void BuildAdvName(const char *device_name)
{
  snprintf(ble_adv_local_name, sizeof(ble_adv_local_name), "HAS3:%s", device_name);
}

void LogMemoryStats(const char *stage)
{
  (void)stage;
}

static bool SendHciCommand(const uint8_t *packet, uint16_t length, uint16_t opcode)
{
  if (packet == nullptr || length < 4 || packet[0] != HCI_PACKET_COMMAND)
  {
    return false;
  }

  hci_command_done = false;
  hci_command_opcode = 0;
  hci_command_status = 0xFF;

  unsigned long started = millis();
  while (!esp_vhci_host_check_send_available())
  {
    if (millis() - started > 50)
    {
      ADV_LOG_VALUE("[ADV] hci send unavailable opcode=0x", String(opcode, HEX));
      return false;
    }
    delay(1);
  }

  esp_vhci_host_send_packet((uint8_t *)packet, length);

  started = millis();
  while (millis() - started <= 500)
  {
    if (hci_command_done && hci_command_opcode == opcode)
    {
      if (hci_command_status != 0x00)
      {
        ADV_LOG_VALUE("[ADV] hci status=", hci_command_status);
      }
      return hci_command_status == 0x00;
    }
    delay(1);
  }
  ADV_LOG_VALUE("[ADV] hci timeout opcode=0x", String(opcode, HEX));
  return false;
}

static bool BuildRawAdvData(uint8_t *buffer, uint8_t *length)
{
  if (buffer == nullptr || length == nullptr || ble_adv_local_name[0] == '\0')
  {
    return false;
  }

  uint8_t offset = 0;
  buffer[offset++] = 2;
  buffer[offset++] = 0x01;
  buffer[offset++] = 0x06;

  uint8_t name_len = (uint8_t)strlen(ble_adv_local_name);
  if (offset + name_len + 2 > HAS3_ADV_DATA_MAX)
  {
    return false;
  }
  buffer[offset++] = name_len + 1;
  buffer[offset++] = 0x09;
  memcpy(buffer + offset, ble_adv_local_name, name_len);
  offset += name_len;

  buffer[offset++] = 2;
  buffer[offset++] = 0x0A;
  buffer[offset++] = 3;

  *length = offset;
  return true;
}

static bool BuildRawScanResponseData(uint8_t *buffer, uint8_t *length)
{
  if (buffer == nullptr || length == nullptr || ble_adv_local_name[0] == '\0')
  {
    return false;
  }

  uint8_t name_len = (uint8_t)strlen(ble_adv_local_name);
  if (name_len + 2 > HAS3_ADV_DATA_MAX)
  {
    return false;
  }

  uint8_t offset = 0;
  buffer[offset++] = name_len + 1;
  buffer[offset++] = 0x09;
  memcpy(buffer + offset, ble_adv_local_name, name_len);
  offset += name_len;

  buffer[offset++] = 2;
  buffer[offset++] = 0x0A;
  buffer[offset++] = 3;

  *length = offset;
  return true;
}

static bool SetAdvertisingEnabled(bool enabled)
{
  uint8_t command[] = {HCI_PACKET_COMMAND, 0x0A, 0x20, 0x01, (uint8_t)(enabled ? 1 : 0)};
  return SendHciCommand(command, sizeof(command), HCI_OPCODE_LE_SET_ADV_ENABLE);
}

static bool ConfigureAdvertisingParams()
{
  uint8_t command[] = {
      HCI_PACKET_COMMAND, 0x06, 0x20, 0x0F,
      (uint8_t)(HAS3_ADV_INTERVAL_UNITS & 0xFF),
      (uint8_t)(HAS3_ADV_INTERVAL_UNITS >> 8),
      (uint8_t)(HAS3_ADV_INTERVAL_UNITS & 0xFF),
      (uint8_t)(HAS3_ADV_INTERVAL_UNITS >> 8),
      0x02,
      0x00,
      0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x07,
      0x00};
  return SendHciCommand(command, sizeof(command), HCI_OPCODE_LE_SET_ADV_PARAMS);
}

static bool ConfigureAdvertisingData()
{
  uint8_t adv_data[HAS3_ADV_DATA_MAX] = {0};
  uint8_t adv_len = 0;
  if (!BuildRawAdvData(adv_data, &adv_len))
  {
    return false;
  }

  uint8_t command[4 + 1 + HAS3_ADV_DATA_MAX] = {0};
  command[0] = HCI_PACKET_COMMAND;
  command[1] = 0x08;
  command[2] = 0x20;
  command[3] = 0x20;
  command[4] = adv_len;
  memcpy(command + 5, adv_data, adv_len);
  return SendHciCommand(command, sizeof(command), HCI_OPCODE_LE_SET_ADV_DATA);
}

static bool ConfigureScanResponseData()
{
  uint8_t response_data[HAS3_ADV_DATA_MAX] = {0};
  uint8_t response_len = 0;
  if (!BuildRawScanResponseData(response_data, &response_len))
  {
    return false;
  }

  uint8_t command[4 + 1 + HAS3_ADV_DATA_MAX] = {0};
  command[0] = HCI_PACKET_COMMAND;
  command[1] = 0x09;
  command[2] = 0x20;
  command[3] = 0x20;
  command[4] = response_len;
  memcpy(command + 5, response_data, response_len);
  return SendHciCommand(command, sizeof(command), HCI_OPCODE_LE_SET_SCAN_RESPONSE_DATA);
}

void BleAdvertiserInit()
{
  if (ble_adv_initialized)
  {
    return;
  }
  // OTA 전용 부팅에서는 BLE 컨트롤러를 켜지 않는다 - GitHub TLS에 그 메모리가 필요하다.
  if (OtaLeanBoot())
  {
    return;
  }

  ADV_LOG("[ADV] init");
  // 컨트롤러 init/enable 은 Arduino 의 btStart() 에 위임한다.
  // btStart() 는 WiFi 공존 초기화(wifi_bt_common_init), Classic 메모리 반납,
  // cfg.mode=BLE 설정, 그리고 컨트롤러 status 전이가 끝날 때까지 대기하는 것까지
  // 처리하므로, 수동 init/enable 보다 안정적으로 VHCI 가 사용 가능해진다.
  if (!btStart())
  {
    ADV_LOG("[ADV] btStart failed");
    ble_adv_retry_pending = true;
    return;
  }

  esp_err_t err = esp_vhci_host_register_callback(&vhci_callbacks);
  if (err != ESP_OK)
  {
    ADV_LOG_VALUE("[ADV] vhci callback err=", err);
    ble_adv_retry_pending = true;
    return;
  }
  ble_adv_initialized = true;

  const uint8_t reset_command[] = {HCI_PACKET_COMMAND, 0x03, 0x0C, 0x00};
  if (!SendHciCommand(reset_command, sizeof(reset_command), HCI_OPCODE_RESET))
  {
    ADV_LOG("[ADV] reset failed");
  }
}

static void StopAdvertising(bool clear_name)
{
  if (ble_adv_initialized && ble_adv_running)
  {
    SetAdvertisingEnabled(false);
  }

  ble_adv_running = false;
  ble_adv_retry_pending = false;
  if (clear_name)
  {
    ble_adv_device_name[0] = '\0';
    ble_adv_local_name[0] = '\0';
  }
}

static bool StartAdvertising()
{
  if (ble_adv_local_name[0] == '\0')
  {
    ADV_LOG("[ADV] no local name");
    return false;
  }

  BleAdvertiserInit();
  if (!ble_adv_initialized)
  {
    ADV_LOG("[ADV] not initialized");
    ble_adv_retry_pending = true;
    return false;
  }

  if (ble_adv_running)
  {
    SetAdvertisingEnabled(false);
    ble_adv_running = false;
  }

  bool ok = ConfigureAdvertisingParams();
  if (!ok)
  {
    ADV_LOG("[ADV] params failed");
  }
  ok = ok && ConfigureAdvertisingData();
  if (!ok)
  {
    ADV_LOG("[ADV] data failed");
  }
  ok = ok && ConfigureScanResponseData();
  if (!ok)
  {
    ADV_LOG("[ADV] scan response failed");
  }
  ok = ok && SetAdvertisingEnabled(true);
  if (!ok)
  {
    ADV_LOG("[ADV] enable failed");
  }

  ble_adv_running = ok;
  ble_adv_retry_pending = !ok;
  if (!ok)
  {
    ble_adv_last_retry_ms = millis();
  }
  else
  {
    ADV_LOG_VALUE("[ADV] advertising ", ble_adv_local_name);
  }
  return ok;
}

void BleAdvertiserUpdateFromDeviceName(const char *device_name)
{
  if (OtaLeanBoot())
  {
    return;
  }
  if (device_name == nullptr || device_name[0] == '\0')
  {
    ADV_LOG("[ADV] missing device_name");
    return;
  }

  if (!Has3IsValidDeviceName(device_name))
  {
    ADV_LOG_VALUE("[ADV] invalid device_name=", device_name);
    StopAdvertising(true);
    return;
  }

  char next_device_name[HAS3_MAX_DEVICE_NAME_LEN + 1] = "";
  strncpy(next_device_name, device_name, sizeof(next_device_name));
  next_device_name[sizeof(next_device_name) - 1] = '\0';

  if (strcmp(ble_adv_device_name, next_device_name) == 0)
  {
    if (!ble_adv_running)
    {
      ble_adv_retry_pending = true;
    }
    return;
  }

  if (ble_adv_device_name[0] != '\0')
  {
    StopAdvertising(false);
  }

  strncpy(ble_adv_device_name, next_device_name, sizeof(ble_adv_device_name));
  ble_adv_device_name[sizeof(ble_adv_device_name) - 1] = '\0';
  BuildAdvName(ble_adv_device_name);
  ADV_LOG_VALUE("[ADV] name=", ble_adv_local_name);
  StartAdvertising();
}

void BleAdvertiserMaintain()
{
  if (OtaLeanBoot())
  {
    return;
  }
  if (!ble_adv_retry_pending || ble_adv_local_name[0] == '\0')
  {
    return;
  }

  unsigned long now = millis();
  if (ble_adv_last_retry_ms != 0 && now - ble_adv_last_retry_ms < HAS3_ADV_RETRY_MS)
  {
    return;
  }

  ble_adv_last_retry_ms = now;
  StartAdvertising();
}
