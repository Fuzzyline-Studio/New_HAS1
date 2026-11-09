// =================================================================================
// dfplayer.ino
// ---------------------------------------------------------------------------------
// DFPlayer Mini(SD카드 기반 MP3 재생 모듈)를 초기화하고 음원을 재생하는 함수 모음.
// 원래는 Nextion 디스플레이로 화면 안내를 하던 것을 DFPlayer 음성 안내로 교체하는 중이라,
// 아래 재생 호출들은 대부분 임시로 (폴더1, 트랙1)만 재생하도록 되어 있다 (TODO 참고).
// =================================================================================
#include "library_and_pin.h"

SoftwareSerial       MP3Serial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN); // DFPlayer와의 UART 통신용 소프트시리얼
DFRobotDFPlayerMini  myDFPlayer;                                  // DFPlayer 제어 객체
bool                 dfPlayerReady = false;                       // 초기화(SD카드 인식) 성공 여부

// setup()에서 1회 호출: DFPlayer와 통신을 열고 초기 볼륨/이퀄라이저/출력 장치를 설정한다.
// 모듈이 응답하지 않거나 SD카드가 없으면 dfPlayerReady를 false로 두고 오디오 없이 계속 진행한다
// (오디오 장애가 게임 진행 자체를 막지 않도록 하기 위함).
void Mp3_Init() {
    MP3Serial.begin(9600);
    Serial.println("[MP3] init start (SD 스캔 대기 2s)");
    delay(2000);  // DFPlayer 전원 인가 후 SD 스캔에 1.5~3초 필요
    myDFPlayer.setTimeOut(1000);
    if (!myDFPlayer.begin(MP3Serial)) {
        dfPlayerReady = false;
        Serial.println("[MP3] init FAIL (배선/SD카드 확인) - 오디오 없이 계속");
        return;
    }
    dfPlayerReady = true;
    Serial.println("[MP3] online");
    myDFPlayer.setTimeOut(500);
    myDFPlayer.volume(30);                        // 볼륨 0~30 중 최대치로 설정
    myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);             // 이퀄라이저: 기본(보정 없음)
    myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);   // 재생 소스: SD카드
}

// SD카드의 (folder_number/file_number) 트랙을 재생한다.
// playLargeFolder는 폴더당 트랙 수가 많은(3자리 파일명, 예: 001~255) 구조를 지원하는 DFPlayer 명령.
// dfPlayerReady가 false면(초기화 실패) 아무 것도 하지 않고 조용히 무시한다.
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number) {
    if (!dfPlayerReady) return;
    DiagnosticScope trace(GeneratorDiagnostic::AudioSend,(uint32_t(folder_number)<<16)|file_number);
    const int32_t track=int32_t((uint32_t(folder_number)<<16)|file_number);
    BlackBoxTrace(GeneratorTrace::AudioBegin,track,0,0); // 0 = send without waiting for finish
    const unsigned long sendStarted=millis();
    BREADCRUMB("Mp3PlayLargeFolder:send");
    myDFPlayer.playLargeFolder(folder_number, file_number);
    BREADCRUMB("Mp3PlayLargeFolder:sent");
    BlackBoxTrace(GeneratorTrace::AudioEnd,track,0,int32_t(millis()-sendStarted));
}

// 트랙 재생이 끝날 때까지(최대 MP3_WAIT_TIMEOUT_MS) 대기한 뒤 리턴한다.
// DFPlayer는 재생 중에 새 play 명령이 오면 즉시 트랙을 끊고 새 트랙으로 전환하므로,
// 여러 음원을 순서대로 들려줘야 하는 자리(예: repaired_all에서 (1,6) 다음 (1,2))에서는
// 앞 트랙을 그냥 Mp3PlayLargeFolder로 재생하면 뒤 호출이 곧바로 끊어버린다 — 이를 막기 위한 함수.
// DFPlayerPlayFinished 신호가 끝내 안 오는 하드웨어 이상 상황에서도 기기가 영영 멈추지 않도록
// timeout을 둔다.
#define MP3_WAIT_TIMEOUT_MS 2000
void Mp3PlayLargeFolderAndWait(uint8_t folder_number, uint16_t file_number) {
    if (!dfPlayerReady) return;
    DiagnosticScope trace(GeneratorDiagnostic::AudioWait,(uint32_t(folder_number)<<16)|file_number);
    const int32_t track=int32_t((uint32_t(folder_number)<<16)|file_number);
    BlackBoxTrace(GeneratorTrace::AudioBegin,track,1,0); // 1 = blocking play until finish/timeout
    BREADCRUMB("Mp3PlayLargeFolderAndWait");
    myDFPlayer.playLargeFolder(folder_number, file_number);
    unsigned long start = millis();
    bool finished=false;
    while (millis() - start < MP3_WAIT_TIMEOUT_MS) {
        if (myDFPlayer.available() && myDFPlayer.readType() == DFPlayerPlayFinished) { finished=true; break; }
    }
    trace.result(finished?1:-1,(uint32_t(folder_number)<<16)|file_number);
    BlackBoxTrace(GeneratorTrace::AudioEnd,track,finished?3:1,int32_t(millis()-start));
    BREADCRUMB("Mp3PlayLargeFolderAndWait:done");
}

// 남은 발전기 개수 안내 — 맞는 음원 트랙이 없어서(재생하면 StartFinish()가 직전에 튼 (1,4)
// "수리 완료" 안내와 겹치거나 엉뚱한 (1,1) "수리 가능" 안내가 이어지던 문제가 있었음) 오디오 재생은
// 하지 않고 로그만 남긴다.
// my["left_generator"]가 1~5 범위를 벗어나면(아직 값이 세팅 안 됐거나 잘못된 값이면) 아무 것도 하지 않는다.
void LeftGenerator() {
    int gen = (int)my["left_generator"];
    if (gen < 1 || gen > 5) return;
    Serial.println("left Generator " + String(gen));
}

// 배선 개수 변화(WirePollMain) 및 상태 동기화(DataChanged, ActivateFunc)마다 호출됨.
// GAUGE 네오픽셀에 실물 배선 비율만 표시한다 — 게이지가 "차오를 때"(배선이 꽂혀 늘어날 때)만
// 나야 하는 음원은 여기서 재생하지 않고, 늘어난 경우인지 판단 가능한 호출부(wire.ino의
// WirePollMain 등)에서 직접 Mp3PlayLargeFolder(1, 7)을 호출한다.
void BatteryPackSend() {
    BatteryGaugeShow(WireDisplayCount(), (int)my["max_battery_pack"]);
}

// Starter tagger feedback waits across loop iterations, so decay and WiFi keep running.
static unsigned long taggerAudioStarted = 0;
void Mp3TaggerStart() {
    if (dfPlayerReady && myDFPlayer.available()) myDFPlayer.readType();
    Mp3PlayLargeFolder(1, 9);
    taggerAudioStarted = millis();
}
bool Mp3TaggerFinished() {
    return !dfPlayerReady || millis() - taggerAudioStarted >= MP3_WAIT_TIMEOUT_MS ||
        (myDFPlayer.available() && myDFPlayer.readType() == DFPlayerPlayFinished);
}

// Battery completion progresses across loop iterations. DFPlayer finish events
// carry no command identity; drain queued events before issuing the new play.
enum class BatteryAudioPhase { Idle, Preparing, Playing };
static BatteryAudioPhase batteryAudioPhase = BatteryAudioPhase::Idle;
static uint32_t batteryAudioStarted = 0;
void Mp3BatteryCancel() { batteryAudioPhase = BatteryAudioPhase::Idle; }
void Mp3BatteryStart() {
    batteryAudioPhase = dfPlayerReady ? BatteryAudioPhase::Preparing : BatteryAudioPhase::Idle;
    batteryAudioStarted = millis();
}
bool Mp3BatteryFinished() {
    if (!dfPlayerReady || batteryAudioPhase == BatteryAudioPhase::Idle) return true;
    if (batteryAudioPhase == BatteryAudioPhase::Preparing) {
        DiagnosticScope trace(GeneratorDiagnostic::AudioDrain);
        for (unsigned i = 0; i < 8; ++i) {
            if (myDFPlayer.available()) myDFPlayer.readType();
            if (!myDFPlayer.hasPendingInput()) break;
        }
        if (myDFPlayer.hasPendingInput()) {
            // Flooded/stuck audio must not prevent the game forever.
            if (uint32_t(millis()-batteryAudioStarted) < MP3_WAIT_TIMEOUT_MS) return false;
            trace.result(-1,uint32_t(millis()-batteryAudioStarted));
            myDFPlayer.resetReceiveState();
            batteryAudioPhase = BatteryAudioPhase::Idle;
            Serial.println("[MP3] battery pre-play event drain timed out; skipping audio");
            return true;
        }
        Mp3PlayLargeFolder(1,3);
        batteryAudioStarted = millis();
        batteryAudioPhase = BatteryAudioPhase::Playing;
        return false;
    }
    for (unsigned i = 0; i < 8 && myDFPlayer.available(); ++i) {
        if (myDFPlayer.readType() == DFPlayerPlayFinished) {
            batteryAudioPhase = BatteryAudioPhase::Idle;
            DiagnosticPoint(GeneratorDiagnostic::AudioWait,1,uint32_t(millis()-batteryAudioStarted));
            DiagnosticPrintf("[GD] t=%lu audio_battery finish_event elapsed=%lu\r\n",(unsigned long)millis(),(unsigned long)uint32_t(millis()-batteryAudioStarted));
            return true;
        }
    }
    if (uint32_t(millis()-batteryAudioStarted) >= MP3_WAIT_TIMEOUT_MS) {
        DiagnosticPoint(GeneratorDiagnostic::AudioWait,-1,uint32_t(millis()-batteryAudioStarted));
        DiagnosticPrintf("[GD] t=%lu audio_battery timeout elapsed=%lu\r\n",(unsigned long)millis(),(unsigned long)uint32_t(millis()-batteryAudioStarted));
        batteryAudioPhase = BatteryAudioPhase::Idle;
        return true;
    }
    return false;
}
