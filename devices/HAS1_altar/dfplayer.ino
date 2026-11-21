#include "HAS1_altar.h"

// 하드웨어 UART2를 기존 핀에 배정한다(HAS1_generator와 같은 방식). 소프트웨어 시리얼은
// 수신 인터럽트 코드를 IRAM에 올리는데, 블랙박스를 넣으면 IRAM이 176바이트 모자라 링크가
// 안 된다. 비트 타이밍도 UART 회로가 맡아 flash 쓰기·Wi-Fi로 CPU가 멈춰도 프레임이 안 깨진다.
HardwareSerial MP3Serial(2);
DFRobotDFPlayerMini myDFPlayer;
bool dfPlayerReady = false;

void Mp3_Init()
{
  MP3Serial.begin(9600, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  delay(2000); // SD카드 스캔 대기

  myDFPlayer.setTimeOut(1000);
  if (myDFPlayer.begin(MP3Serial))
  {
    dfPlayerReady = true;
    myDFPlayer.setTimeOut(500);
    myDFPlayer.volume(30);
    myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
    myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
    Serial.println("DFPlayer connected successfully");
  }
  else
  {
    Serial.println("!!!DFPlayer connect failed!!! - continuing anyway");
  }
}

void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
{
  if (!dfPlayerReady) return;
  myDFPlayer.playLargeFolder(folder_number, file_number);
}
