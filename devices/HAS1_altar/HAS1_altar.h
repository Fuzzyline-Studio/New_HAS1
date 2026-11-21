#ifndef _HAS1_ALTAR_H_
#define _HAS1_ALTAR_H_

#include "library_and_pin.h"
#include "location_protocol.h"
#include "recovery_policy.h"

//============================ Global Variable ============================
void NeoNo();
void (*NeoFunc)() = NeoNo;

//================================ Wifi ==================================
void TelnetInit();
void TelnetLoop();

HAS2_Wifi has2wifi("http://172.30.1.43");

SecureOTA ota(
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/update.bin",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/version.txt",
  "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/update.sig",
  HMAC_SECRET,
  FIRMWARE_VER
);

bool activate_bool;

void SettingFunc();
void ReadyFunc();
void ActionFunc();
void DataChange();
void CrashReportInit();
void CrashReportSend(const char *device_name);
void OtaBootInit();
bool OtaLeanBoot();

//* =============================== Sensor =============================== *
/**
 * @brief Temple에 사용되는 센서, 모듈 세팅
 */
void SensorInit();
void BleAdvertiserInit();
void BleAdvertiserUpdateFromDeviceName(const char *device_name);
void BleAdvertiserMaintain();
void LogMemoryStats(const char *stage);

//=============================== Solenoid ================================
void SolenoidInit();
void SolenoidOn();
void SolenoidOff();
void SolenoidPulse();

//============================== IR Sensor =================================
// 생명칩이 투입구를 통과하면 감지 (device 로직: sensor.ino)
void IrSensorInit();
void IrSensorLoop();

//============================= Micro Switch ================================
// 회전 메커니즘이 한바퀴 돌면 눌림 → 솔레노이드 개방 트리거 (sensor.ino)
void MicroSwInit();
void MicroSwLoop();

//=============================== DFPlayer (MP3) ============================
void Mp3_Init();
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number);

//================================ RFID ==================================
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

bool rfid_tag = false;
byte rfid_tag_count = 0; // 몇번 태그 됐는지 (= 덕트를 몇 번 사용했는지) 확인하는 변수

bool send_nfc_err = false;

// PN532 근접 인식 Dead Zone 대응 — RxGain 동적 전환 (HAS1_tagmachine_sub와 동일 기법, sensor.ino 구현).
// GainMode는 반드시 여기(헤더)서 정의해야 한다 — Arduino가 .ino 탭들을 병합할 때 자동 생성하는
// 함수 프로토타입을 스케치 맨 앞(이 헤더 include 다음, 각 .ino의 실제 코드보다 앞)에 삽입하므로,
// sensor.ino 안에서만 정의하면 그 프로토타입 자리에서 "GainMode를 아직 모른다"는 컴파일 에러가 난다.
enum GainMode { GAIN_NEAR, GAIN_FAR };

// ntag2xx_ReadPage는 한 페이지(4바이트)만 반환한다. page7의 GxPx 코드 길이.
static constexpr size_t RFID_PAGE_DATA_SIZE = 4;

// 유지 중이던 태그가 두 Gain 모두에서 이 시간 이상 연속으로 안 잡히면 그제서야 제거로 판정.
// tagmachine_sub(연속 폴링, 500ms)와 달리 altar의 RfidLoop는 1초에 한 번만 도므로, 단발성
// 미스 한 번(=최대 1000ms 공백)도 흡수할 수 있게 폴링 주기의 2배 이상으로 잡는다.
#define TAG_REMOVE_TIME_MS 2500

// InListPassiveTarget 재시도 횟수. PN532 기본값 0xFF는 태그가 올 때까지 무한 탐색이라
// 호스트가 타임아웃으로 포기한 뒤에도 칩은 계속 바쁘고, 거기에 다음 명령을 얹으면
// 프레임 위상이 어긋나 리더가 "죽은 것처럼" 멈추는 원인이 됐다. 유한 재시도여야
// readPassiveTargetID의 타임아웃 안에 PN532가 응답을 끝내고 다음 명령을 받는다.
// (HAS1_tagmachine_sub와 같은 값 - 그쪽에서 실측 검증됨)
#define RFID_ACTIVATION_RETRIES 10
#define RFID_DETECT_TIMEOUT_MS 250

bool RfidInit(void);
void RfidLoop(void);
void RfidMaintain(void);                      // loop()마다 호출 - 예약된 PN532 재초기화를 백오프에 맞춰 수행
void RequestRfidReinit(const char *reason);   // ACK 필수 명령이 실패하면 호출 - 리더를 못 믿는 상태로 전환
void CardChecking(uint8_t rfidData[RFID_PAGE_DATA_SIZE]);
void RunAltarSuccess(); // MicroSwLoop에서 태그+칩 확인 후 솔레노이드 개방 전에 집계 요청

//=============================== Neopixel ===============================
// TODO 네오픽셀 개수 확인
#define NUMPIXELS_SQUARE 15   // NEO_SQUARE1
#define NUMPIXELS_ROUND 24
#define NUMPIXELS_SIDE 96
#define NUMPIXELS_SQUARE2 15  // NEO_SQUARE2 — 실측 전까지 추정치
#define NUMPIXELS_PN532   8   // NEO_PN532 — 실측 전까지 추정치
Adafruit_NeoPixel pixels_square(NUMPIXELS_SQUARE, NEOPIXEL_PIN_SQUARE, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_round(NUMPIXELS_ROUND, NEOPIXEL_PIN_ROUND, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_side(NUMPIXELS_SIDE, NEOPIXEL_PIN_SIDE, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_square2(NUMPIXELS_SQUARE2, NEOPIXEL_PIN_SQUARE2, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixels_pn532(NUMPIXELS_PN532, NEOPIXEL_PIN_PN532, NEO_GRB + NEO_KHZ800);

// DEFAULT_BRIGHTNESS: 부팅 직후 및 서버 brightness 값이 유효 범위(1~100)를 벗어날 때
// 적용하는 기준 밝기 (raw 0~255). 다른 HAS1 device와 동일한 값으로 맞춘다.
#define DEFAULT_BRIGHTNESS 50

// Neopixel 색상정보 (다른 장치들과 동일하게 풀스케일 255 기준 — 실제 밝기는 setBrightness()가 담당)
int black[3] = {0, 0, 0};
int white[3] = {255, 255, 255};
int red[3] = {255, 0, 0};
int yellow[3] = {255, 255, 0};
int green[3] = {0, 255, 0};
int purple[3] = {255, 0, 255};
int purple_white[3] = {255, 128, 255}; // purple + white 중간톤 (blink 진입 시 side용)

void applyBrightness();
void lightColor(Adafruit_NeoPixel &pixels, int color[3]);
void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index);
void lightRgb(Adafruit_NeoPixel &pixels, int r, int g, int b);
void ClearSquare();  // pixels_square + pixels_square2(둘째 사각 링) 동시 클리어
void ClearRound();   // pixels_round + pixels_pn532(태그 지점 링) 동시 클리어

void NeoBeforeTagger();
void NeoTagger();
void NeoTaggerTag();
void NeoAfterTagger();
void NeoGaming();
void NeoChipGaugeBlink();      // blink: 생명칩 태그 시 round+square가 함께 차오르는 게이지
void NeoChipBlinkActivate();   // activate: 생명칩 태그(+1) 시 round+square가 깜빡임
void NeoTakenChip();
void NeoWin();
void NeoLose();

//================================ Timer =================================
// 1초마다 RFID 가 인식되게 타이머 설정
SimpleTimer rfid_timer;
SimpleTimer nsec_tag_timer;
SimpleTimer wifi_timer;

int rfid_timer_id;
int nsec_tag_timer_id;
int wifi_timer_id;

int nsec_tag_num;
bool nsec_tag_bool;

void TimerInit();
void TimerRun();
void RfidTimerAssess();
void RfidTagTimerFunc();
void WifiTimerFunc();
void NsecTagTimerFailFunc();
void NsecTagTimerSuccessFunc();

#endif
