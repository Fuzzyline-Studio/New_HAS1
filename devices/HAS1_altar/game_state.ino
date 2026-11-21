#include "HAS1_altar.h"

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
            // OTA 다운로드+플래시는 30초 넘게 걸릴 수 있어(그 동안 esp_task_wdt_reset()이
            // 안 불림) 워치독을 그대로 두면 업데이트 도중 재부팅으로 끊긴다(HAS1_generator에서
            // 실측된 문제, 동일 워치독 구성이라 여기도 잠재해 있었음). ota.check() 동안만
            // 이 태스크를 워치독에서 빼고, 끝나면 다시 등록한다(성공 시엔 자체 재부팅하므로
            // 재등록까지 안 감).
            esp_task_wdt_delete(NULL);
            BlackBoxOtaBegin(); // MQTT 세션 정리 대기(최대 15초) + flash 쓰기 정지
            ota.check();
            BlackBoxOtaEnd();
            esp_task_wdt_add(NULL);
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