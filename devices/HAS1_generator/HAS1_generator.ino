#include <HAS2_Wifi.h>
#include <SecureOTA.h>
#include <nvs_flash.h>
#include <WiFi.h>
#include <Network.h>
#include <esp_task_wdt.h>

/**
 * @file Done_Generator_code.ino
 * @author 김병준 (you@domain.com)
 * @brief
 * @version 1.0
 * @date 2022-11-29
 *
 * @copyright Copyright (c) 2022
 *
 */

// FIRMWARE_VER / PARTITION_VER: OTA가 "새 버전이 있는지" 판단하는 기준값.
// GitHub Releases에 올라간 version.txt / partition_version.txt와 비교되며,
// 배포할 때마다 이 숫자를 올려야 기기가 업데이트를 인식한다.
#define FIRMWARE_VER 42
#define PARTITION_VER 1
#include "HAS1_generator.h"

static bool generatorTcpIpReady = false;

// ---------------------------------------------------------------------------------
// setup(): 전원이 켜지거나 리셋될 때 1회만 실행되는 초기화 루틴.
// 각 모듈(네오픽셀/RFID/엔코더/배선/DFPlayer/타이머)을 순서대로 초기화한 뒤,
// WiFi 연결과 OTA 콜백을 등록하고, 마지막으로 게임 상태 머신을 대기 상태로 세팅한다.
// ---------------------------------------------------------------------------------
void setup() {
    BlackBoxBegin(); // Durable logger starts before blocking peripheral initialization.
    DiagnosticInit(); // Capture previous RTC evidence before any initialization can block.
    Serial.begin(115200);
    DebugSerial.println(GeneratorBle::bootMessage());
    has2wifi.SetHttpDiagnosticHook(DiagnosticHttpHook);
    has2wifi.EnableBoundedResponseBody(); // Reject stalled/oversized HTTP bodies before they block the loop watchdog.
    has2wifi.SetDebugPrint(&DebugSerial); // 라이브러리/비동기 통신 로그도 같은 UART + RAM/Telnet 경로로 전달
    CrashReportInit();  // 직전 재부팅이 워치독/패닉이었다면 마지막 위치를 로그로 남김 (crash.ino)
    TelnetPreserveCrashReport(); // URL용 가공/최근 로그 덮어쓰기 전에 직전 리셋 요약 원문 보관
    NeopixelInit();  // 네오픽셀 4개 스트립 초기화 (전체 흰색 점등)
    RfidInit();      // PN532 RFID 리더 초기화
    EncoderInit();   // 엔코더용 하드웨어 펄스 카운터(PCNT) 초기화
    WireInit();      // 배선 감지 핀 4개 INPUT_PULLUP 설정
    Mp3_Init();      // DFPlayer(MP3) 모듈 초기화
    TimerInit();     // SimpleTimer 3종 등록(및 정지)

    // BeginConnection only configures state; one network worker performs Wi-Fi
    // recovery and HTTP while main remains responsive. Preserve NVS crash data.
    ptrCurrentMode = WaitFunc;
    ptrRfidMode = WaitFunc;
    // NetworkServer::begin() opens an lwIP socket without initializing TCP/IP.
    // Initialize on main before the worker can concurrently enter WiFi.mode().
    // Wi-Fi association remains asynchronous; no connection or delay is needed.
    generatorTcpIpReady = Network.begin();
    if (!generatorTcpIpReady) {
        Serial.println("[NET] TCP/IP initialization failed; network startup disabled");
        return;
    }
    has2wifi.BeginConnection("badland");
    GeneratorNetworkBegin();
    TelnetInit();

    // OTA 진행 로그를 시리얼로 출력하도록 설정
    ota.setLogStream(Serial);

    // OTA 성공 시: 서버에 "setting" 상태를 알리고 재부팅 시작을 로그로 남김
    ota.setOnSuccess([]() {
        GeneratorNetworkSend(my["device_name"] | "", "device_state", "setting");
        Serial.println("[OTA] update succees! Start rebooting...");
    });
    // OTA 스킵(이미 최신 버전) 시: 마찬가지로 "setting" 상태를 알림
    ota.setOnSkip([]() {
        GeneratorNetworkSend(my["device_name"] | "", "device_state", "setting");
        Serial.println("[OTA] Already up-to-date version.");
    });

    // 파티션 테이블(partitions.bin) 자체를 갱신해야 하는 경우를 위한 별도 OTA 채널.
    // PARTITION_VER이 서버의 partition_version.txt보다 낮을 때만 적용된다.
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_generator/partition_version.txt",
        PARTITION_VER
    );

    // 게임 상태 머신을 "아무 것도 안 함" 상태로 초기화
    ptrCurrentMode = WaitFunc;
    ptrRfidMode = WaitFunc;

    // 초기 서버 상태는 GeneratorNetworkLoop에서 비동기 응답을 받은 뒤 적용한다.
    GeneratorBle::begin();
}

// ---------------------------------------------------------------------------------
// loop(): 전원이 켜져 있는 동안 반복 실행되는 메인 루프.
// 매 프레임마다 (1) 엔코더 하드웨어 카운터를 읽어 반영하고,
// (2) 현재 게임 모드에 해당하는 함수를 실행하고,
// (3) 등록된 SimpleTimer들을 갱신한다.
// 블로킹(delay 등)을 최소화해 WiFi 통신·네오픽셀 갱신·엔코더 반영이 최대한 끊기지 않게 한다.
// ---------------------------------------------------------------------------------
void loop() {
    DiagnosticLoop();
    BlackBoxLoop();
    NeoRetryPending();
    if (!generatorTcpIpReady) {
        // Keep reset evidence readable over UART and yield without opening any
        // sockets, starting network work, or repeatedly rebooting on init failure.
        esp_task_wdt_reset(); // TimerInit already registered this main task.
        delay(1);
        return;
    }
    GeneratorNetworkLoop();
    TelnetRun();        // Telnet 클라이언트 접속/데이터 처리
    WireServiceLoop(); // Sample/synchronize before any starter progress or completion.
    EncoderLoop();     // PCNT 하드웨어 카운터 → encoderValue 반영
    // 서버 응답에 의한 상태 전환은 NetworkLoop에서 인코더 초기화 전에 세션을 닫는다.
    // 로컬 모드 이탈은 직전 게이지 값으로 여기서 정리한다. 게임 세대가 바뀐 세션은
    // 폐기하며, 기여도 전송도 큐에만 등록하므로 메인 루프에서 응답을 기다리지 않는다.
    ContribLoop();
    ptrCurrentMode();  // 현재 모드 함수 실행 (예: WaitFunc / RfidLoopMain / WirePollMain / StarterActivate)
    TimerRun();         // 완료 기준 HTTP 폴링 예약 및 GameTimer/BlinkTimer 갱신
    // 엔코더/배선/서버 처리가 끝난 뒤 BLE 설정을 대기 없이 한 단계만 진행한다.
    GeneratorBle::poll();
}
