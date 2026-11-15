// =================================================================================
// library_and_pin.h
// ---------------------------------------------------------------------------------
// 이 프로젝트(HAS1_generator, "발전기" 기기)에서 사용하는 모든 외부 라이브러리와
// ESP32 GPIO 핀 번호를 한 곳에 모아 정의하는 헤더 파일.
// 다른 .ino 탭들은 전부 이 파일을 (간접적으로) include 해서 핀 번호/라이브러리를 사용한다.
// =================================================================================
#ifndef _LIBRARY_AND_PIN_
#define _LIBRARY_AND_PIN_

#include <Arduino.h>

#include <Adafruit_PN532.h>     // RFID/NFC 리더(PN532) 제어 라이브러리
#include <Adafruit_NeoPixel.h>  // WS2812 계열 네오픽셀(LED 스트립) 제어 라이브러리

#include "DFRobotDFPlayerMini.h"  // MP3 음원 재생 모듈(DFPlayer Mini) 제어 라이브러리
#include <HAS2_Wifi.h>            // 서버와 통신(상태 송수신)하는 사내 공용 WiFi 라이브러리
#include "generator_ble.h"
#include <SimpleTimer.h>          // millis() 기반 소프트웨어 인터벌 타이머 라이브러리
#include <SecureOTA.h>            // 서명 검증 기반 OTA(원격 펌웨어 업데이트) 라이브러리
#include "secrets.h"              // OTA 서명 검증용 HMAC 비밀키 (별도 관리, git에 커밋되는 값은 아님)
#include <esp_task_wdt.h>         // 태스크 워치독 — loop()가 멈추면 자동 재부팅 (timer.ino)
#include "crash.h"                // BREADCRUMB/크래시 로그 — 워치독 재부팅 직전 위치를 서버에 보고 (crash.ino)

// ---------------------------------------------------------------------------------
// DFPlayer Mini (MP3 재생 모듈) 통신 핀
// ESP32 기준(하드웨어 UART2에 배정)이므로 RX/TX는 DFPlayer 쪽 TX/RX와 교차 연결됨
// ---------------------------------------------------------------------------------
#define DFPLAYER_RX_PIN 39
#define DFPLAYER_TX_PIN 33

// ---------------------------------------------------------------------------------
// PN532 RFID 리더 (SPI 통신) 핀
// ---------------------------------------------------------------------------------
#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS1 5   // Slave Select (Chip Select) — 리더가 1개(rfid_num = 1)라 SS1만 사용

// ---------------------------------------------------------------------------------
// 네오픽셀(WS2812) 데이터 핀 — 용도별로 4개 스트립이 분리되어 있음
// (GAUGE / STARTER / DEVICESTATE / CIRCUIT, 자세한 매핑은 HAS1_generator.h 참고)
// ---------------------------------------------------------------------------------
#define PN532_NEOPIXEL_PIN 25    // 게이지(배선 충전량 / 스타터 진행률) 표시용
#define ENCODER_NEOPIXEL_PIN 26  // 스타터(엔코더) 상태 표시용
#define INNER_NEOPIXEL_PIN 27    // 기기 내부 상태 표시용
#define CIRCUIT_NEOPIXEL_PIN 14  // 회로/외곽 표시용

#define RELAY_PIN  14   // (참고) CIRCUIT_NEOPIXEL_PIN과 핀 번호가 겹침 — 현재 릴레이는 미사용 코드로 추정

// ---------------------------------------------------------------------------------
// 엔코더(손잡이를 돌려 발전기를 수리하는 스타터 단계) 입력 핀 — A상/B상
// 실제 카운팅은 ISR이 아니라 ESP32 하드웨어 PCNT로 처리 (encoder.ino 참고)
// ---------------------------------------------------------------------------------
#define encoderPinA 13
#define encoderPinB 15

// ---------------------------------------------------------------------------------
// 배선(와이어) 감지 핀 — 배터리팩 충전 단계에서 사용
// 평소에는 INPUT_PULLUP으로 풀업되어 HIGH(1, floating 상태)이고,
// 배선(데이터선+GND)을 꽂으면 GND로 당겨져 LOW(0)가 된다.
// 예전 MOTOR_INA1_PIN(32)/BOXSWITCH_PIN(36)/buttonPin(34) 자리를 재사용함
// (MOTOR_INA1은 코드상 항상 LOW라 하드웨어에서 GND 직결로 대체, 나머지 둘은 원래 미사용 상태였음).
// ---------------------------------------------------------------------------------
#define WIRE_PIN_1 32
#define WIRE_PIN_2 34
#define WIRE_PIN_3 35
#define WIRE_PIN_4 36

#endif

// Separate blackbox library. Empty broker URI records locally only. Set enabled
// to 0 for baseline benchmarks; never use credentials inside the broker URI.
#ifndef HAS1_GENERATOR_BLACKBOX_ENABLED
#define HAS1_GENERATOR_BLACKBOX_ENABLED 1
#endif
#ifndef HAS1_GENERATOR_BLACKBOX_BROKER_URI
#define HAS1_GENERATOR_BLACKBOX_BROKER_URI ""
#endif

#ifndef HAS1_GENERATOR_BLACKBOX_MQTT_USERNAME
#define HAS1_GENERATOR_BLACKBOX_MQTT_USERNAME ""
#endif
#ifndef HAS1_GENERATOR_BLACKBOX_MQTT_PASSWORD
#define HAS1_GENERATOR_BLACKBOX_MQTT_PASSWORD ""
#endif
