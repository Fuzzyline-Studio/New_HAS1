#ifndef _LIBRARY_AND_PIN_H_
#define _LIBRARY_AND_PIN_H_
// 라이브러리 선언

#include <Wire.h>
#include <SPI.h>
#include <Esp.h>
#include <Arduino.h>

#include <HAS2_Wifi.h>

#include <Adafruit_NeoPixel.h>
#include <Adafruit_PN532.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

#include <SimpleTimer.h>
#include <esp_task_wdt.h>

#include <SecureOTA.h>
#include "secrets.h"

// 별도 블랙박스 라이브러리(HAS1BlackBox). broker URI가 비어 있으면 flash에만 남긴다.
// 정식 배포는 CI가 secrets.h에 broker URI/암호를 써넣는다(계정 이름은 장치 MAC).
// 0으로 두면 기록/업로드 태스크를 만들지 않는다(훅 호출만 남는다).
#ifndef HAS1_ALTAR_BLACKBOX_ENABLED
#define HAS1_ALTAR_BLACKBOX_ENABLED 1
#endif
#ifndef HAS1_ALTAR_BLACKBOX_BROKER_URI
#define HAS1_ALTAR_BLACKBOX_BROKER_URI ""
#endif
#ifndef HAS1_ALTAR_BLACKBOX_MQTT_USERNAME
#define HAS1_ALTAR_BLACKBOX_MQTT_USERNAME ""
#endif
#ifndef HAS1_ALTAR_BLACKBOX_MQTT_PASSWORD
#define HAS1_ALTAR_BLACKBOX_MQTT_PASSWORD ""
#endif
#include "blackbox.h"


// 핀 선언

// #define SERIAL1_RX_PIN 36 // 미사용
// #define SERIAL1_TX_PIN 32

#define NEOPIXEL_PIN_SQUARE 25      // NEO_SQUARE1
#define NEOPIXEL_PIN_ROUND  26      // NEO_ROUND
#define NEOPIXEL_PIN_SIDE   27      // NEO_SIDE
#define NEOPIXEL_PIN_SQUARE2 32     // NEO_SQUARE2
#define NEOPIXEL_PIN_PN532   22     // NEO_PN532

#define PN532_SCK                       (18)
#define PN532_MISO                      (19)
#define PN532_MOSI                      (23)
#define PN532_SS                        (5)

#define DFPLAYER_RX_PIN 39   // ESP RX ← DFPlayer TX
#define DFPLAYER_TX_PIN 33   // ESP TX → DFPlayer RX

#define SOLENOID_PIN 14                 // 솔레노이드 데이터 핀 (MOSFET IRLZ44N 구동)
#define SOLENOID_PULSE_MS 2000          // 잠금/해제 순간 통전 길이(ms). 500ms는 딸깍만 나고 안 열림

#define IR_SENSOR_PIN 34                // 생명칩 투입 감지 (감지 시 LOW)
#define IR_SENSOR_DEBOUNCE_MS 200

#define MICRO_SW_PIN 35                 // 회전 메커니즘 완료 감지 (외부 10K 풀업, 평소 HIGH, 눌리면 LOW)
#define MICRO_SW_DEBOUNCE_MS 10

#include "crash.h"
#include "telnet.h"
// Redirect Serial → TelnetSerial so all Serial.print/println also go to telnet
#define Serial SerialMirror

#endif