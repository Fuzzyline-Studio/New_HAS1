/**
 * @file HAS1_altar.ino
 * @author YuBin Kim
 * @brief
 * @version 0.1
 * @date 2022-11-24 ~ 2022-11-26
 *
 * @copyright Copyright (c) 2022
 */

#define FIRMWARE_VER 51
#define PARTITION_VER 3
#include "HAS1_altar.h"
// loopTask 기본 스택(8KB)으로는 WiFi+BLE+RFID+Neopixel x5 조합의 setup()
// 호출 체인에서 스택 오버플로우(Stack canary watchpoint triggered)가 발생해 16KB로 증설.
// arduino-esp32 core의 weak 심볼을 오버라이드하는 방식 (시그니처 반드시 일치해야 함).
size_t getArduinoLoopTaskStackSize(void)
{
  return 16384;
}

//************************************************ Core1 ********************************************************************
/**
 * @brief Temple Intialize
 */
void TempleInit()
{
  has2wifi.SetDebugPrint(&SerialMirror);  // 라이브러리 내부 로그도 Serial+Telnet 양쪽으로
  has2wifi.Setup("badland");                                      // 와이파이 세팅
  LogMemoryStats("Wi-Fi connected");
  TelnetInit();
  // 다른 장치와 동일하게 부팅 시 현재 펌웨어 버전을 서버에 보고한다.
  has2wifi.Send((String)(const char *)my["device_name"], "esp_version", String(FIRMWARE_VER));
  BleAdvertiserInit();
  ota.setLogStream(Serial);
  ota.setOnSuccess([]() {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  });
  ota.setOnSkip([]() {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  });
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_altar/partition_version.txt",
        PARTITION_VER
    );
  SensorInit();                                                      // IoT Glove 사용 센서, 모듈 세팅
  TimerInit();                                                       // 타이머 세팅
  
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜지면 한번만 실행)
 */
void setup()
{
  SolenoidInit();
  delay(1000);
  Serial.begin(115200);
  SerialMirror.printf("[boot] firmware=%u\n", static_cast<unsigned>(FIRMWARE_VER));
  CrashReportInit();
  OtaBootInit(); // 직전 부팅이 GitHub OTA를 요청했는지 판정 (game_state.ino)
  if (!OtaLeanBoot())
  {
    BlackBoxBegin(); // 부팅 배출과 Wi-Fi/RFID 초기화(막힐 수 있음)보다 먼저 기록을 시작한다
  }
  LogMemoryStats("boot");
  SerialMirror.println("[boot] Solenoid purge for 2000 ms (no chip count)");
  SolenoidPulse();
  TempleInit();
  DataChange();
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜져있는동안 Core1에서 계속 실행)
 */
void loop()
{
  TelnetLoop();
  BlackBoxLoop();
  TimerRun();
  NeoFunc();
  IrSensorLoop();
  MicroSwLoop();
  RfidMaintain(); // 예약된 PN532 재초기화만 처리 (정상일 땐 즉시 반환)
  if (activate_bool)
  {
    ActivateFunc();
  }
}
