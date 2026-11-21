#include "HAS1_altar.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>

//****************************************** GitHub OTA 전용 부팅 ******************************************
// GitHub OTA는 TLS에 약 45KiB(연속 17KiB 블록 2개 포함)가 필요하다. BLE와 블랙박스가 켜진
// 정상 부팅에서는 그만큼 남지 않을 수 있어(HAS1_generator에서 실제로 모든 요청이 실패했다),
// device_state가 github이 되면 표시를 남기고 재부팅한 뒤 BLE·블랙박스 없이 뜬 부팅에서
// ota.check()를 돌린다. 업데이트가 없거나 실패하면 다시 재부팅해 정상 부팅으로 돌아온다.
// 표시는 RTC no-init에 두어 소프트웨어 재부팅만 건너간다(전원 차단이면 사라진다).
static const uint32_t OTA_BOOT_REQUEST = 0x4f544131; // 다음 부팅은 OTA 전용
static const uint32_t OTA_BOOT_DONE = 0x4f544132;    // 이번 github 지시는 이미 시도함
static RTC_NOINIT_ATTR uint32_t ota_boot_marker;
static bool ota_lean_boot = false;

void OtaBootInit()
{
    const bool software_reset = esp_reset_reason() == ESP_RST_SW;
    ota_lean_boot = software_reset && ota_boot_marker == OTA_BOOT_REQUEST;
    if (ota_lean_boot)
    {
        ota_boot_marker = OTA_BOOT_DONE;
    }
    else if (!software_reset || ota_boot_marker != OTA_BOOT_DONE)
    {
        ota_boot_marker = 0; // 전원 투입/패닉 뒤의 값은 믿지 않는다
    }
}

bool OtaLeanBoot()
{
    return ota_lean_boot;
}

// loop 워치독을 뗀 채 SecureOTA가 자체 타임아웃을 넘겨 멈추면 장치가 그대로 서 버린다.
static void OtaStallRestart(void *)
{
    esp_restart();
}

static void OtaStallGuard()
{
    esp_timer_create_args_t args = {};
    args.callback = OtaStallRestart;
    args.name = "ota-stall";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK)
    {
        esp_timer_start_once(timer, 5ULL * 60ULL * 1000000ULL);
    }
}

static void GithubOta()
{
    if (ota_lean_boot)
    {
        Serial.printf("[OTA] lean boot: heap=%lu largest=%lu\n",
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        // OTA 다운로드+플래시는 30초 넘게 걸릴 수 있어(그 동안 esp_task_wdt_reset()이
        // 안 불림) 워치독을 그대로 두면 업데이트 도중 재부팅으로 끊긴다. 성공하면
        // ota.check()가 직접 재부팅한다.
        esp_task_wdt_delete(NULL);
        OtaStallGuard();
        ota.check();
        // 업데이트가 없거나 실패했다. BLE는 부팅 때만 켤 수 있으므로 재부팅해 정상으로 돌아간다.
        Serial.println("[OTA] no update applied - restarting into normal mode");
        delay(200);
        ESP.restart();
    }
    if (ota_boot_marker == OTA_BOOT_DONE)
    {
        // 이번 github 지시는 OTA 전용 부팅에서 이미 시도했다. 서버 상태가 github에 남아
        // 있어도(실패 시 device_state를 못 돌려놓음) 재부팅을 반복하지 않는다.
        Serial.println("[OTA] github already attempted - waiting for device_state to change");
        BlackBoxAltar("ota.github.skipped", 0, 0, 0, 0);
        return;
    }
    Serial.println("[OTA] github requested - restarting into OTA-only mode");
    BlackBoxAltar("ota.github.reboot", (int32_t)ESP.getFreeHeap(),
                  (int32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), 0, 0);
    ota_boot_marker = OTA_BOOT_REQUEST;
    delay(200);
    ESP.restart();
}

/**
 * @brief DB gamestate가 setting 일 때 한번동작하는 코드
 */
void SettingFunc()
{
    activate_bool = false;
    NeoFunc = NeoNo;
    lightColor(pixels_round, white);
    lightColor(pixels_side, white);
    lightColor(pixels_square, white);
}

/**
 * @brief DB gamestate가 ready 일 때 한번동작하는 코드
 */

void ReadyFunc()
{
    activate_bool = false;
    NeoFunc = NeoBeforeTagger;
}

/**
 * @brief DB gamestate가 activate 일 때 반복동작하는 코드
 */
void ActivateFunc()
{
    RfidLoop();
}

/**
 * @brief DB gamestate가 activate 일 때 한번동작하는 코드
 */
void ActivateRunOnce()
{
    activate_bool = true;
}

void DataChange()
{
    const char *device_name = (const char *)my["device_name"];
    BleAdvertiserUpdateFromDeviceName(device_name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  

    if (!device_name)
    {
        Serial.println("[DataChange] No server data yet, skipping");
        return;
    }

    // JsonDocument(크기 템플릿 없는 v7 타입) 사용 — StaticJsonDocument<N>은 N이 my와 정확히
    // 같아야만 대입(operator=)이 되는데, 로컬/CI에 깔린 HAS2_Wifi 사본마다 my의 선언 크기가
    // 다를 수 있어(예: 1000 vs 2048) 매번 컴파일 에러가 났다(다른 device들과 동일 이슈).
    static JsonDocument cur;

    bool any_change = false;

    // 밝기 먼저 반영 — 이어지는 상태 전환이 새 밝기로 칠해지도록. 변경 감지·로그는 applyBrightness() 내부.
    applyBrightness();

    if ((String)(const char *)my["game_state"] != (String)(const char *)cur["game_state"])
    {
        Serial.println("[DataChange] game_state: " +
            (String)(const char *)cur["game_state"] + " -> " +
            (String)(const char *)my["game_state"]);
        any_change = true;
        BlackBoxAltarState(0, (const char *)my["game_state"]);
        if ((String)(const char *)my["game_state"] == "setting")
        {
            SettingFunc();
        }
        else if ((String)(const char *)my["game_state"] == "ready")
        {
            ReadyFunc();
        }
        else if ((String)(const char *)my["game_state"] == "activate")
        {
            ActivateRunOnce();
        }
    }

    if ((String)(const char *)my["device_state"] != (String)(const char *)cur["device_state"])
    {
        Serial.println("[DataChange] device_state: " +
            (String)(const char *)cur["device_state"] + " -> " +
            (String)(const char *)my["device_state"]);
        any_change = true;
        BlackBoxAltarState(1, (const char *)my["device_state"]);
        if ((String)(const char *)my["device_state"] != "github")
        {
            ota_boot_marker = 0; // github을 벗어났다 - 다음 github 지시는 새로 시도한다
        }
        if ((String)(const char *)my["device_state"] == "activate")
        {
            NeoFunc = NeoGaming;
            // 태그 지점(pn532)/side는 activate 진입 시 한 번만 보라색으로 고정한다.
            // NeoGaming() 안에서 매 loop마다 다시 켜면(반복 재전송) 깜빡여 보이는
            // 문제가 있어서, round/square 애니메이션과 분리해 여기서 한 번만 설정.
            lightColor(pixels_pn532, purple);
            lightColor(pixels_side, purple);
            NeoGaming(); // 상단(round/square)도 진입 즉시 켠다 - 다음 loop까지 미루면 side보다 늦게 켜져 보인다
        }
        else if ((String)(const char *)my["device_state"] == "player_win")
        {
            //NeoFunc = NeoLose;
        }
        else if ((String)(const char *)my["device_state"] == "player_lose")
        {
            //NeoFunc = NeoWin;
        }
        else if ((String)(const char *)my["device_state"] == "blink")
        {
            NeoFunc = NeoTagger;
            activate_bool = true;
            // blink 진입 시 side를 보라+흰색 중간톤으로 한 번만 켜둔다(NeoTagger는 side를 안 건드림).
            lightColor(pixels_side, purple_white);
        }
        else if ((String)(const char *)my["device_state"] == "github")
        {
            GithubOta(); // 정상 부팅이면 OTA 전용 부팅으로 재시작, OTA 전용 부팅이면 ota.check()
        }
    }

    if((int)my["taken_chip"] != (int)cur["taken_chip"])
    {
        Serial.println("[DataChange] taken_chip: " +
            String((int)cur["taken_chip"]) + " -> " + String((int)my["taken_chip"]));
        any_change = true;
        BlackBoxAltar("altar.taken.max", (int)my["taken_chip"], (int)my["max_chip"], 0, 0);
    }
    if((int)my["max_chip"] != (int)cur["max_chip"])
    {
        Serial.println("[DataChange] max_chip: " +
            String((int)cur["max_chip"]) + " -> " + String((int)my["max_chip"]));
        any_change = true;
    }

    if (any_change)
        Serial.println("[DataChange] applied");

    cur = my;
}