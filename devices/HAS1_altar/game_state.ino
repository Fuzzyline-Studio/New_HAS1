#include "HAS1_altar.h"
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "ota_boot_policy.h"

//****************************************** GitHub OTA 전용 부팅 ******************************************
// GitHub OTA는 TLS에 약 45KiB(연속 17KiB 블록 2개 포함)가 필요하다. BLE와 블랙박스가 켜진
// 정상 부팅에서는 그만큼 남지 않을 수 있어(HAS1_generator에서 실제로 모든 요청이 실패했다),
// device_state가 github이 되면 요청을 남기고 재부팅한 뒤 BLE·블랙박스 없이 뜬 부팅에서
// ota.check()를 돌린다. 업데이트가 없거나 실패하면 다시 재부팅해 정상 부팅으로 돌아온다.
// 언제 요청하고 언제 그만두는지는 ota_boot_policy.h (호스트 테스트 대상).
static RTC_NOINIT_ATTR OtaBootState ota_boot_state;
static bool ota_lean_boot = false;

// OTA 전용 부팅은 5분을 넘기지 않는다. loop 워치독을 뗀 채 SecureOTA가 멈추거나, 서버에
// 닿지 못해 상태를 영영 못 받아도 BLE·블랙박스 없는 채로 남지 않게 한다.
static void OtaBootDeadline(void *)
{
    esp_restart();
}

void OtaBootInit()
{
    ota_lean_boot = OtaBootPolicy::begin(ota_boot_state, esp_reset_reason() == ESP_RST_POWERON);
    if (!ota_lean_boot) return;
    esp_timer_create_args_t args = {};
    args.callback = OtaBootDeadline;
    args.name = "ota-boot";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK)
    {
        esp_timer_start_once(timer, 5ULL * 60ULL * 1000000ULL);
    }
}

bool OtaLeanBoot()
{
    return ota_lean_boot;
}

static void GithubOta()
{
    if (ota_lean_boot)
    {
        // 이 부팅은 BLE를 켜지 않았다. 컨트롤러용으로 잡혀 있는 메모리도 heap으로 돌려준다
        // (컨트롤러를 초기화하기 전에만 가능 - 이 부팅은 어차피 재부팅으로 끝난다).
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        Serial.printf("[OTA] OTA-only boot %lu/%lu: heap=%lu largest=%lu\n",
                      (unsigned long)ota_boot_state.attempts,
                      (unsigned long)OtaBootPolicy::kMaxAttempts,
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        // OTA 다운로드+플래시는 30초 넘게 걸릴 수 있어(그 동안 esp_task_wdt_reset()이
        // 안 불림) 워치독을 그대로 두면 업데이트 도중 재부팅으로 끊긴다. 성공하면
        // ota.check()가 직접 재부팅한다. 멈추면 OtaBootDeadline이 재부팅한다.
        esp_task_wdt_delete(NULL);
        ota.check();
        // 업데이트가 없거나 실패했다. BLE는 부팅 때만 켤 수 있으므로 재부팅해 정상으로 돌아간다.
        Serial.println("[OTA] no update applied - restarting into normal mode");
        delay(200);
        ESP.restart();
    }
    if (!OtaBootPolicy::mayRequest(ota_boot_state))
    {
        // 이번 github 지시로 이미 한도만큼 시도했다. 서버 상태가 github에 남아 있어도
        // (실패하면 device_state를 못 돌려놓는다) 재부팅을 반복하지 않는다.
        Serial.println("[OTA] github attempts used up - waiting for device_state to change");
        BlackBoxAltar("ota.github.skipped", (int32_t)ota_boot_state.attempts, 0, 0, 0);
        return;
    }
    Serial.println("[OTA] github requested - restarting into OTA-only mode");
    BlackBoxAltar("ota.github.reboot", (int32_t)ESP.getFreeHeap(),
                  (int32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (int32_t)ota_boot_state.attempts, 0);
    OtaBootPolicy::request(ota_boot_state);
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

    // 여기까지 왔으면 서버 상태가 github이 아니다(github이면 위에서 재부팅했거나 한도 초과로
    // 건너뛰었다). 한도 초과로 건너뛴 경우는 횟수를 지우지 않는다.
    if ((String)(const char *)my["device_state"] != "github")
    {
        OtaBootPolicy::leftGithub(ota_boot_state);
        if (ota_lean_boot)
        {
            // OTA 전용 부팅인데 할 OTA가 없다 - BLE·블랙박스 없이 게임을 돌리지 않는다.
            Serial.println("[OTA] device_state is not github - restarting into normal mode");
            delay(200);
            ESP.restart();
        }
    }

    cur = my;
}