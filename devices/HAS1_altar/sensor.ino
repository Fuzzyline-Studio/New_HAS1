#include "HAS1_altar.h"

// 술래 활성화(pn532 태그) + 생명칩 투입(IR센서), 둘 다 충족돼야 발동. 어느 쪽이
// 먼저든 상관없이(RFID는 ~1초마다만 폴링되므로 IR감지가 먼저 올 수도 있다) 나중에
// 확인되는 쪽에서 성공 처리한다. device_state는 항상 "activate"로 고정 - 여기서
// 바꾸지 않는다. (RfidLoop/CardChecking보다 먼저 선언돼야 함 - Arduino는 함수
// 프로토타입만 자동 생성하고 변수는 안 해줘서, 사용부보다 파일 앞쪽에 있어야 한다.)
static bool tag_active = false;
static unsigned long tag_start_time = 0;
static String pending_tagger_device_name = "";
static bool ir_chip_pending = false;          // IR센서가 칩을 감지해서 크랭크 대기 중
static bool ir_chip_confirm_animated = false; // 이 칩(ir_chip_pending 세션)에 대해 OnTagChipConfirmed(애니메이션)를 이미 실행했는지
// 술래가 훔친 칩을 반납하는 기존 메커닉(아래 두번째 분기)도 태그가 붙어있는 동안
// 디바운스 없이 매초(RfidLoop 폴링 주기) 블로킹 HTTP 3개(has2wifi.Send)를 반복
// 발사했었다 - 그동안 loop()가 막혀 MicroSw 클릭을 놓치는 원인이 됐다. 한 번만
// 처리하도록 tag_active와 같은 방식으로 게이트.
static bool chip_return_processed = false;
// 태그가 리더에 붙어있는 동안 RfidLoop가 ~1초마다 CardChecking을 다시 호출하는데,
// 그때마다 has2wifi.Receive()(블로킹 HTTP)를 재발사하면 그 응답을 기다리는 동안
// loop()가 멈춰 MicroSwLoop의 디지털 엣지 샘플링을 놓친다 - 마이크로스위치는
// 인터럽트가 아니라 매 loop 1회 폴링이라 이 타이밍에 클릭이 나면 통째로 씹혀서
// 여러 번 돌려야 인식되는 원인이 됐다. 같은 태그 세션에서는 한 번만 조회한다.
static bool tag_data_fetched = false;
// PN532 재초기화 상태(SensorInit/RfidLoop/RfidMaintain 공용 - 사용부보다 앞에 둬야 한다).
// ready()가 false인 동안 RfidLoop는 PN532를 건드리지 않고 "태그 없음"으로 처리한다
// (리더를 못 믿는 동안 크랭크가 솔레노이드를 열지 못하게 - fail-closed).
static RfidRecoveryPolicy rfid_recovery;

// 태그+칩이 (순서 상관없이) 둘 다 확인된 순간 - 로컬 애니메이션만 즉시 트리거해서
// "확인됐으니 크랭크를 돌리라"는 피드백을 준다. taken_chip 갱신/tagger_name
// 브로드캐스트(서버 반영)는 여기서 하지 않는다 - 태그+칩만 확인되고 크랭크를 안
// 돌리면 점수가 올라가면 안 되므로, 실제 반영은 RunAltarSuccess()가 맡고 그건
// MicroSwLoop가 솔레노이드를 열기 전에 호출한다.
void OnTagChipConfirmed()
{
  Serial.println("[Altar] Tag + chip confirmed together - waiting for crank");
  // 애니메이션은 delay() 없이 NeoFunc()가 매 loop마다 조금씩 진행시킨다.
  NeoFunc = NeoChipBlinkActivate;
}

// 크랭크 입력과 태그+칩 조건이 확인되면 솔레노이드를 열기 전에 호출한다.
// taken_chip 갱신을 요청한다. Send()는 반환값이 없어 반영 성공을 보장하지 않는다.
// (tagger_name 전체 브로드캐스트는
// 더 이상 쓰지 않아 제거 - 플레이어 8명한테 매번 블로킹 HTTP를 쐈었음.)
void RunAltarSuccess()
{
  Serial.println("[Altar] Crank turned -> requesting taken_chip +1 before solenoid opens");
  const unsigned long count_started = millis();
  BlackBoxAltar("altar.count.begin", 0, 0, 0, 0);
  has2wifi.Send((String)(const char *)my["device_name"], "taken_chip", "+1");
  // Send()는 결과를 돌려주지 않는다 - 걸린 시간만 남긴다(서버 반영 여부의 증거가 아님).
  BlackBoxAltar("altar.count.end", (int32_t)(millis() - count_started), 0, 0, 0);
}

//****************************************** Initialize ******************************************
void SensorInit()
{
  // Neopixel init
  pixels_square.begin();
  pixels_round.begin();
  pixels_side.begin();
  pixels_square2.begin();
  pixels_pn532.begin();
  // 서버 brightness 수신 전까지 적용할 기준 밝기
  pixels_square.setBrightness(DEFAULT_BRIGHTNESS);
  pixels_round.setBrightness(DEFAULT_BRIGHTNESS);
  pixels_side.setBrightness(DEFAULT_BRIGHTNESS);
  pixels_square2.setBrightness(DEFAULT_BRIGHTNESS);
  pixels_pn532.setBrightness(DEFAULT_BRIGHTNESS);

  // Rfid init - 실패해도 멈추지 않고 loop()의 RfidMaintain이 백오프로 계속 재시도한다.
  // (예전엔 부팅 때 한 번 실패하면 리더 없이 영원히 돌았다)
  if (RfidInit())
  {
    rfid_recovery.recordSuccess();
  }
  else
  {
    Serial.println("!!!RFID connect failed!!! - will keep retrying in loop()");
    // 서버가 github을 지시한 상태면 보고하지 않는다: device_state를 덮어쓰면 OTA 전용
    // 부팅이 github을 못 보고 정상 부팅으로 돌아가 OTA가 취소된다(리더가 죽은 제단이야말로
    // 펌웨어를 받아야 한다). OTA 전용 부팅에서도 같은 이유로 보내지 않는다.
    if (!OtaLeanBoot() && (String)(const char *)my["device_state"] != "github")
    {
      has2wifi.Send((String)(const char *)my["device_name"], "device_state", "PN532");
    }
    rfid_recovery.recordFailure(millis());
  }

  // 솔레노이드는 setup()에서 초기화/부팅 배출 완료. IR / Micro Switch init
  IrSensorInit();
  MicroSwInit();
}

//********************************************* Rfid *********************************************
// PN532 근접 인식 Dead Zone 대응 — RxGain 동적 전환. 일부 생산 로트의 PN532는 기본
// RxGain(38dB)에서 태그 중심 정렬 시 ~2cm 이하 근거리에서 인식이 안 되는 특성이 실측으로
// 확인됨(동일 TTGO/배선/태그/펌웨어에서 PN532만 교체해 재현). RxGain을 낮추면(23dB) 근거리가,
// 높이면(33dB) 중거리(2~4cm)가 커버되어 두 세팅을 상황에 따라 전환해 근접~4cm를 잇는다.
// (HAS1_tagmachine_sub/rfid.ino와 동일 기법 - 그쪽에서 실측 검증됨)
static GainMode pn532_gain = GAIN_NEAR;
static bool pn532_tag_locked = false;      // 태그를 찾아 유지 중인지 (탐색 모드 vs 유지 모드)
static uint8_t pn532_locked_data[RFID_PAGE_DATA_SIZE];       // 유지 중인 태그의 page7 데이터 - 동일 태그 판별 기준
static unsigned long pn532_last_seen_ms = 0; // 유지 중 태그를 마지막으로 확인한 시각

// RFConfiguration(0x32) CfgItem 0x0A(Type A 106kbps Analog Setting)로 RxGain을 전환한다.
// PN532는 이 설정을 내부에 영구 저장하지 않으므로, 초기화(전원 재투입/모듈 교체 포함)마다
// 다시 적용해야 한다(RfidInit 참고). TX 관련 값(GsNOn/CWGsP)은 실측상 근거리 개선 기여가
// 낮아 PN532 기본값을 그대로 둔다.
static bool ApplyGain(GainMode mode)
{
  uint8_t rfCfg = (mode == GAIN_NEAR) ? 0x19 : 0x49; // 23dB(근거리) / 33dB(중거리)
  uint8_t cmd[] = {
      0x32,       // RFConfiguration
      0x0A,       // Type A 106kbps Analog Setting
      rfCfg,      // RFCfg — RxGain
      0xF4,       // GsNOn
      0x3F,       // CWGsP
      0x11,       // ModGsP
      0x4D,       // Demod RF ON
      0x85,       // RxThreshold
      0x61,       // Demod RF OFF
      0x6F,       // GsNOff
      0x26,       // ModWidth
      0x62,       // MifNFC
      0x87        // TxBitPhase
  };
  BREADCRUMB("RfidLoop:applyGain");
  return nfc.sendCommandCheckAck(cmd, sizeof(cmd), 1000);
}

// 현재 Gain으로 태그 감지 + page7 읽기를 1회 시도한다.
// readPassiveTargetID()는 InListPassiveTarget의 ACK와 결과 프레임을 모두 소비한다.
// 예전의 0x00(정의되지 않은 명령) 핑 + startPassiveTargetIDDetection() 조합은 결과
// 프레임을 읽지 않은 채 바로 InDataExchange를 보내 PN532 프레임 위상을 깨뜨릴 수 있었고,
// 무한 재시도(기본 0xFF) 탓에 태그가 없으면 칩이 탐색에서 영영 못 빠져나왔다.
// 타임아웃은 RFID_ACTIVATION_RETRIES번의 탐색이 끝나기에 충분해야 한다(그래야 응답이
// 남지 않는다). "태그 없음"과 "ACK 없음"은 여기서 구분하지 않는다 - 리더 생존 여부는
// ACK가 반드시 와야 하는 ApplyGain()의 결과로 RfidLoop가 판정한다.
static bool DetectAndRead(uint8_t outData[RFID_PAGE_DATA_SIZE])
{
  // 라이브러리는 응답 프레임의 길이 바이트(uint8_t, 최대 255)만큼 uid를 검사 없이 복사한다.
  // 어긋난 프레임이 오면 7바이트 버퍼로는 스택이 깨져 게임 중 패닉이 나므로 최대치로 잡는다.
  uint8_t uid[256];
  uint8_t uidLength = 0;
  BREADCRUMB("RfidLoop:detectTarget");
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, RFID_DETECT_TIMEOUT_MS)) return false;
  BREADCRUMB("RfidLoop:readPage");
  return nfc.ntag2xx_ReadPage(7, outData) != 0;
}

/**
 * @brief RFID(=PN532) 세팅. 실패하면 false - 호출자가 재시도 일정을 잡는다.
 *        (부팅 시 SensorInit, 실행 중 RfidMaintain 양쪽에서 쓴다)
 */
bool RfidInit(void)
{
  BREADCRUMB("RfidInit");
  // pn532_tag_locked/pn532_locked_data는 건드리지 않는다 - 재초기화 뒤 같은 태그가 그대로
  // 붙어있으면 유지 모드가 이어져 반납 보상(chip_return_processed)이 다시 지급되지 않는다.
  // 죽은 모듈이면 begin()의 wakeup(SAMConfig 대기 1초) + getFirmwareVersion(1초)로 약 2초 막힌다.
  nfc.begin(); // 소프트 SPI 핀 재설정 + wakeup (RSTPDN이 배선돼 있지 않아 하드웨어 리셋은 없음)
  if (!(nfc.getFirmwareVersion()))
  {
    Serial.println("[RFID] PN532 firmware query failed");
    return false;
  }
  if (!nfc.SAMConfig())
  {
    Serial.println("[RFID] PN532 SAMConfig failed");
    return false;
  }
  // 기본 0xFF는 카드가 올 때까지 InListPassiveTarget을 무한 수행한다. 유한 재시도를
  // 설정해야 readPassiveTargetID의 timeout 뒤에도 PN532가 다음 명령을 받을 수 있다.
  if (!nfc.setPassiveActivationRetries(RFID_ACTIVATION_RETRIES))
  {
    Serial.println("[RFID] PN532 activation retry setup failed");
    return false;
  }
  // PN532는 RF 설정을 저장하지 않으므로 초기화(전원 재투입/모듈 교체 포함)마다 재적용한다.
  pn532_gain = GAIN_NEAR;
  if (!ApplyGain(pn532_gain))
  {
    Serial.println("[RFID] PN532 gain setup failed");
    return false;
  }
  Serial.println("RFID connected successfully");
  return true;
}

// 리더 상태 변화를 서버에도 남긴다. device_state는 건드리지 않는다 - 게임 중에
// activate/blink를 덮어쓰면 엔진·대시보드가 오동작하고, 복구돼도 되돌릴 근거가 없다.
// 대신 제단만 쓰는 자유 형식 crash_log 컬럼을 재사용한다(WifiTimerFunc의
// CrashReportSend가 다음 2초 주기에 한 번 보낸다. 공백/특수문자는 그쪽에서 치환).
// 진짜 크래시 보고(재부팅 직후 CrashReportInit이 세움)가 아직 안 나갔으면 덮어쓰지 않는다 -
// 그 경우 리더 이벤트는 Serial/Telnet 로그에만 남는다.
static const char *rfid_fault_reason = "";
static void RfidReportEvent(const char *event, const char *detail, unsigned fails)
{
  char msg[sizeof(g_crash_msg)];
  snprintf(msg, sizeof(msg), "pn532=%s %s uptime=%lums fails=%u",
           event, detail, (unsigned long)millis(), fails);
  Serial.printf("[RFID] %s\n", msg);
  if (g_crash_pending) return;
  strncpy(g_crash_msg, msg, sizeof(g_crash_msg) - 1);
  g_crash_msg[sizeof(g_crash_msg) - 1] = '\0';
  g_crash_pending = true;
}

/**
 * @brief ACK가 반드시 와야 하는 명령이 실패했을 때 호출 - 리더를 못 믿는 상태로 전환하고
 *        재초기화를 예약한다. 즉시 재초기화하지 않는 이유: 죽은 모듈에 RfidInit을 돌리면
 *        약 2초를 통째로 막히므로 loop()의 RfidMaintain이 백오프 간격으로 시도한다.
 *        태그 유지 상태(pn532_tag_locked)는 그대로 둔다 - 복구 뒤 같은 태그면 세션이 이어진다.
 */
void RequestRfidReinit(const char *reason)
{
  rfid_fault_reason = reason;
  rfid_recovery.request(millis());
  RfidReportEvent("reinit_requested", reason, 0);
  BlackBoxAltar("altar.rfid.reinit", 0, 0, 0, 0);
}

/**
 * @brief loop()마다 호출. 재초기화가 예약돼 있고 재시도 시각이 됐을 때만 RfidInit을 돌린다.
 *        정상 상태(ready)에서는 비교 한 번 하고 바로 돌아온다.
 */
void RfidMaintain(void)
{
  unsigned long now = millis();
  if (!rfid_recovery.attemptDue(now)) return;

  Serial.println("[RFID] re-initializing PN532...");
  if (RfidInit())
  {
    unsigned fails = rfid_recovery.failureCount();
    rfid_recovery.recordSuccess();
    RfidReportEvent("recovered", rfid_fault_reason, fails);
    BlackBoxAltar("altar.rfid.recovered", (int32_t)fails, 0, 0, 0);
    // 부팅 때 실패해서 우리가 PN532 상태를 보냈던 경우에만 되돌린다(게임 중 상태는 건드리지 않음).
    if ((String)(const char *)my["device_state"] == "PN532")
    {
      has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
    }
    return;
  }
  rfid_recovery.recordFailure(millis());
  Serial.printf("[RFID] PN532 retry in %lu ms\n",
                (unsigned long)RfidRecoveryPolicy::retryDelayMs(rfid_recovery.failureCount()));
}

/**
 * @brief RFID 태그 인식 (근거리/중거리 Gain 자동 전환 + 단발성 미스 디바운스)
 */
void RfidLoop()
{
  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(1000, RfidTagTimerFunc);
  }
  else
  {
    return;
  }

  uint8_t data[RFID_PAGE_DATA_SIZE] = {0};
  bool tag_present = false;

  if (!rfid_recovery.ready())
  {
    // 리더 재초기화 대기 중 - PN532를 건드리지 않는다(죽은 모듈에 명령을 보내면 매번
    // 타임아웃까지 loop가 막힌다). 아래 공통 처리에서 "태그 없음"으로 게이트를 내려
    // 리더를 못 믿는 동안엔 크랭크를 돌려도 솔레노이드가 열리지 않게 한다.
  }
  else if (!pn532_tag_locked)
  {
    // 탐색 모드: 현재 Gain으로 1회 시도 → 실패하면 반대 Gain으로 즉시 재시도
    //   (23dB↔33dB를 오가며 근접~4cm 전 구간을 커버, 둘 다 실패하면 이번 tick은 미검출)
    // ApplyGain은 태그 유무와 무관하게 ACK가 와야 하는 명령이라, 여기서 실패하면
    // 리더 자체가 응답하지 않는 것 - 재초기화를 예약한다(태그 없는 매 tick마다 검사됨).
    if (!DetectAndRead(data))
    {
      GainMode otherGain = (pn532_gain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
      if (!ApplyGain(otherGain))
      {
        RequestRfidReinit("gain_switch_failed");
      }
      else
      {
        pn532_gain = otherGain;
        pn532_tag_locked = DetectAndRead(data);
      }
    }
    else
    {
      pn532_tag_locked = true;
    }

    if (pn532_tag_locked)
    {
      memcpy(pn532_locked_data, data, sizeof(pn532_locked_data));
      pn532_last_seen_ms = millis();
      tag_present = true;
      CardChecking(data);
    }
  }
  else
  {
    // 유지 모드: 현재 Gain으로 먼저 확인 → 실패하면 반대 Gain으로 즉시 재확인
    //   → 그래도 둘 다 실패하면 TAG_REMOVE_TIME_MS 동안은 유지로 간주(단발성 미스 무시)
    //   → 유예시간 초과 시에만 최종적으로 태그 제거 판정, 이후 탐색 모드로 복귀
    bool found = DetectAndRead(data) && memcmp(data, pn532_locked_data, sizeof(pn532_locked_data)) == 0;
    if (!found)
    {
      GainMode otherGain = (pn532_gain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
      if (!ApplyGain(otherGain))
      {
        RequestRfidReinit("gain_switch_failed");
      }
      else if (DetectAndRead(data) && memcmp(data, pn532_locked_data, sizeof(pn532_locked_data)) == 0)
      {
        pn532_gain = otherGain; // 반대 Gain에서 같은 태그 재확인 → 그 Gain으로 전환해 유지
        found = true;
      }
      else if (!ApplyGain(pn532_gain)) // 재확인 실패 — 칩 설정을 원래 Gain으로 되돌려 상태 일치시킴
      {
        RequestRfidReinit("gain_restore_failed");
      }
    }

    if (!rfid_recovery.ready())
    {
      // 위에서 재초기화가 예약됨 - 유예시간 로직을 타지 않고 즉시 태그 없음으로 처리
      // (리더가 죽었는데 2.5초 동안 "태그 유지 중"으로 솔레노이드가 열리면 안 된다).
    }
    else if (found)
    {
      pn532_last_seen_ms = millis();
      tag_present = true;
      CardChecking(pn532_locked_data);
    }
    else if (millis() - pn532_last_seen_ms < TAG_REMOVE_TIME_MS)
    {
      // 유예시간 이내 - 단발성 미스로 보고 이번 tick은 CardChecking 없이 유지 상태만 지속
      tag_present = true;
    }
    else
    {
      // 유예시간 초과 - 태그 제거 확정, 탐색 모드로 복귀
      pn532_tag_locked = false;
      // 제거가 확정된 이 시점에 세션 게이트를 직접 내린다. 아래 공통 리셋은 리더가 살아있을
      // 때만 도는데, 바로 다음 게인 리셋이 실패하면 락은 이미 풀린 채 게이트만 남아 복구 뒤
      // 첫 카드가 이전 플레이어의 조회 데이터(tag JSON)로 처리되는 구멍이 생긴다.
      chip_return_processed = false;
      tag_data_fetched = false;
      pn532_gain = GAIN_NEAR;
      if (!ApplyGain(pn532_gain))
      {
        RequestRfidReinit("gain_reset_failed");
      }
    }
  }

  // Tagger left the reader without inserting a chip - reset and ignore.
  // (리더 장애 중에도 내린다 - 솔레노이드 게이트라 확인 못 하면 닫혀 있어야 한다)
  if (!tag_present && tag_active)
  {
    Serial.println("[RFID] Tag left reader - reset, ignored");
    BlackBoxAltar("altar.tag.left", rfid_recovery.ready(), 0, 0, 0);
    tag_active = false;
  }
  // 태그가 리더에서 떠났으면 반납 처리 게이트도 같이 리셋 (다음에 다시 태그하면 재처리 가능해야 함).
  // 단, 리더 장애(재초기화 대기)로 못 본 것은 "떠남"이 아니다 - 여기서 리셋하면 복구 직후 같은
  // 태그에 taken_chip/exp 반납 보상이 한 번 더 지급된다. 제거 판정은 리더가 살아난 뒤 유지 모드의
  // 유예시간 로직(또는 다른 카드 감지)이 내린다.
  if (!tag_present && rfid_recovery.ready())
  {
    chip_return_processed = false;
    tag_data_fetched = false;
  }
  BREADCRUMB("RfidLoop:done");
}

/**
 * @brief RFID에 태그된 NFC의 데이터에 따른 코드 동작
 *
 * @param rfidData 태그된 NFC의 데이터
 */
void CardChecking(uint8_t rfidData[RFID_PAGE_DATA_SIZE]) // 어떤 카드가 들어왔는지 확인용
{
  BREADCRUMB("CardChecking:recv");
  String tagUser = "";
  for (size_t i = 0; i < RFID_PAGE_DATA_SIZE; i++) // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Serial.println("tag_user_data : " + tagUser);

  // 1. 태그한 플레이어의 역할과 생명칩갯수, 최대생명칩갯수 등 읽어오기.
  // 같은 물리 태그가 리더에 계속 붙어있는 동안은 값이 바뀔 일이 없으므로 세션당
  // 한 번만 조회한다(반복 조회 시 블로킹 HTTP로 MicroSw 클릭을 놓치는 문제 방지).
  if (!tag_data_fetched)
  {
    const unsigned long fetch_started = millis();
    has2wifi.Receive(tagUser);
    tag_data_fetched = true;
    // 플레이어 이름은 남기지 않는다: 조회에 걸린 시간, 술래 여부, 제단 game_state만.
    BlackBoxAltar("altar.tag.fetch", (int32_t)(millis() - fetch_started),
                  (String)(const char *)tag["role"] == "tagger",
                  (String)(const char *)my["game_state"] == "activate", 0);
  }

  // 2. 술래 태그 확인 - device_state/서버 호출 없이 내부 플래그만 세운다.
  // 태그가 계속 붙어있는 동안 반복 감지돼도(리더가 ~1초마다 다시 읽음) 한 번만 처리.
  if ((String)(const char *)my["game_state"] == "activate" && (String)(const char *)tag["role"] == "tagger")
  {
    if (!tag_active)
    {
      tag_active = true;
      tag_start_time = millis();
      pending_tagger_device_name = (String)(const char *)tag["device_name"];
      Serial.println("[Altar] Tagger tag detected, waiting for chip insertion: " + pending_tagger_device_name);
      BlackBoxAltar("altar.tag.active", ir_chip_pending, 0, 0, 0);

      // 칩이 태그보다 먼저 감지돼서 이미 대기 중이었다면(RFID는 ~1초마다만 폴링돼서
      // IR감지가 먼저 올 수 있음), 지금 태그로 뒤늦게 완성된 것 - 확인 애니메이션만
      // 트리거한다(서버 반영은 크랭크가 실제로 돌아갈 때 RunAltarSuccess가 처리).
      // (game_state=="activate"는 이미 위에서 확인됨 - device_state는 blink든
      // activate든 상관없이 태그+칩+크랭크면 열려야 한다.)
      // tag_active는 여기서 false로 되돌리지 않는다 - 태그는 여전히 리더에 붙어있고,
      // 되돌리면 다음 폴링(~1초 뒤)에 "새로 태그됨"으로 오인해서 애니메이션이
      // 매초 반복 트리거되는 버그가 있었다.
      if (ir_chip_pending && !ir_chip_confirm_animated)
      {
        ir_chip_confirm_animated = true;
        OnTagChipConfirmed();
      }
    }
    // already tag_active -> do nothing, avoid re-running while tag stays on reader
  }

  if (!chip_return_processed && (String)(const char *)tag["role"] == "tagger" && (int)tag["taken_chip"] > 0)
  {
    // if(RfidNsecTag(2))
    // {
    // 3. 태그한 사용자가 술래이면서 빼앗은 칩을 1개 이상 가지고 있다면
    chip_return_processed = true; // 태그가 붙어있는 동안 반복 실행(블로킹 HTTP 반복 발사) 방지
    String altar_state = (String)(const char *)my["device_state"];

    BREADCRUMB("CardChecking:chipSend");
    has2wifi.Send((String)(const char *)my["device_name"], "taken_chip", "+1");
    has2wifi.Send((String)(const char *)tag["device_name"], "exp", "+100");

    // 애니메이션은 delay()로 막지 않고 NeoFunc()에 맡겨 매 loop마다 조금씩 진행시킨다
    // (그래야 이 태그 처리 중에도 IrSensorLoop/MicroSwLoop가 계속 돌 수 있다).
    if (altar_state == "blink")
    {
      NeoFunc = NeoChipGaugeBlink; // round + square(1,2)가 함께 차오르는 게이지
    }
    else if (altar_state == "activate")
    {
      NeoFunc = NeoChipBlinkActivate; // round + square(1,2)가 깜빡이는 연출
    }
  }
  //}
}

bool RfidNsecTag(int sec)
{
  if (nsec_tag_num == 0 && !nsec_tag_bool)
  {
    nsec_tag_timer_id = nsec_tag_timer.setTimeout(5000, NsecTagTimerFailFunc);
    nsec_tag_bool = true;
  }
  else
  {
    nsec_tag_timer.restartTimer(nsec_tag_timer_id);
  }

  if (nsec_tag_num >= sec && nsec_tag_bool)
  {
    Serial.println("Tag success");
    nsec_tag_timer.deleteTimer(nsec_tag_timer_id);
    nsec_tag_bool = false;
    nsec_tag_timer_id = nsec_tag_timer.setTimeout(2000, NsecTagTimerSuccessFunc);
    return true;
  }
  else
  {
    nsec_tag_num++;
  }
  return false;
}

//******************************************* Neopixel Helpers *******************************************
void applyBrightness()
{
  // 값이 그대로면 건너뛴다 — 변경 감지를 호출부가 아니라 여기서 한다 (전 device 공통 방식).
  static int prevServerBrightness = -1;  // -1: 첫 호출은 반드시 적용
  int b = (int)my["brightness"];
  if (b == prevServerBrightness) return;
  Serial.println("[DataChange] brightness: " + String(prevServerBrightness) + " -> " + String(b));
  prevServerBrightness = b;
  int brightness;
  if (b <= 0 || b > 100)
    brightness = DEFAULT_BRIGHTNESS;
  else
    brightness = map(b, 0, 100, 0, 255);
  pixels_square.setBrightness(brightness);
  pixels_round.setBrightness(brightness);
  pixels_side.setBrightness(brightness);
  pixels_square2.setBrightness(brightness);
  pixels_pn532.setBrightness(brightness);
  NeoShow(pixels_square);
  NeoShow(pixels_round);
  NeoShow(pixels_side);
  NeoShow(pixels_square2);
  NeoShow(pixels_pn532);
}

// pixels_square2(두번째 사각 링)/pixels_pn532(태그 지점 링)는 물리적으로 각각
// pixels_square/pixels_round의 짝이라, 기존 호출부를 다 건드리는 대신 헬퍼에서
// 자동으로 같은 색을 미러링한다. clear()는 헬퍼를 안 거치므로 ClearSquare/ClearRound로 대체.
void lightColor(Adafruit_NeoPixel &pixels, int color[3])
{
  pixels.fill(pixels.Color(color[0], color[1], color[2]));
  NeoShow(pixels);
  if (&pixels == &pixels_square) lightColor(pixels_square2, color);
  else if (&pixels == &pixels_round) lightColor(pixels_pn532, color);
}

void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index)
{
  pixels.setPixelColor(index, color[0], color[1], color[2]);
  NeoShow(pixels);
  if (&pixels == &pixels_round) lightColor(pixels_pn532, color, index % NUMPIXELS_PN532);
  else if (&pixels == &pixels_square) lightColor(pixels_square2, color, index);
}

void ClearSquare()
{
  pixels_square.clear();
  pixels_square2.clear();
}

void ClearRound()
{
  pixels_round.clear();
  pixels_pn532.clear();
}

void lightRgb(Adafruit_NeoPixel &pixels, int r, int g, int b)
{
  pixels.fill(pixels.Color(r, g, b));
  NeoShow(pixels);
  if (&pixels == &pixels_square) lightRgb(pixels_square2, r, g, b);
  else if (&pixels == &pixels_round) lightRgb(pixels_pn532, r, g, b);
}

//******************************************* Neopixel *******************************************
void NeoNo()
{
}

// 칩 확인 연출(NeoChipGaugeBlink/NeoChipBlinkActivate)의 진행 상태. 함수 안 static이면
// 크랭크로 연출을 도중에 끊었을 때 다음 연출이 끊긴 지점부터 이어져 보이므로 파일 범위로
// 빼서 NeoSceneOff()가 되돌릴 수 있게 한다.
static int chip_gauge_step = 0;
static unsigned long chip_gauge_next_ms = 0;
static bool chip_blink_on = false;
static int chip_blink_count = 0;
static unsigned long chip_blink_next_ms = 0;

static void NeoAnimReset()
{
  chip_gauge_step = 0;
  chip_gauge_next_ms = 0;
  chip_blink_on = false;
  chip_blink_count = 0;
  chip_blink_next_ms = 0;
}

// 크랭크 통전 전후 LED 처리. 통전 2초는 delay()라 loop가 멈추고 LED는 그 직전 상태로
// 고정된다 - 플레이어가 태그+칩 직후 바로 크랭크를 돌리면 확인 연출이 도중에 얼어붙은
// 채 2초를 보내고 남은 칸부터 이어져 어색했고, 솔레노이드와 LED 전류가 그 2초 동안 겹쳤다.
// 통전 직전에 모든 스트립을 끄고(피크 전류 최소화 + 의도된 암전), 통전이 끝나면 현재 서버
// 상태의 기본 화면으로 되돌린다. 별도 성공 연출은 두지 않는다.
void NeoSceneOff()
{
  NeoFunc = NeoNo; // 진행 중이던 연출 중단 - 복귀 시 상태별 기본 연출로 다시 지정한다
  NeoAnimReset();
  pixels_round.clear();   NeoShow(pixels_round);
  pixels_pn532.clear();   NeoShow(pixels_pn532);
  pixels_square.clear();  NeoShow(pixels_square);
  pixels_square2.clear(); NeoShow(pixels_square2);
  pixels_side.clear();    NeoShow(pixels_side);
  // 암전은 통전 전에 반드시 나가야 한다 - 블랙박스 flash 쓰기에 밀린 스트립을 마저 보낸다.
  NeoFlushPending(100);
}

// DataChange()의 상태 진입 처리와 같은 화면으로 되돌린다 (그쪽을 바꾸면 여기도 맞출 것).
void NeoSceneRestore()
{
  String game_state = (String)(const char *)my["game_state"];
  String device_state = (String)(const char *)my["device_state"];
  if (game_state == "setting")
  {
    NeoFunc = NeoNo; // SettingFunc와 동일: 전체 흰색 고정
    lightColor(pixels_round, white);
    lightColor(pixels_side, white);
    lightColor(pixels_square, white);
  }
  else if (game_state == "ready")
  {
    NeoFunc = NeoBeforeTagger; // 매 loop 빨간색
  }
  else if (device_state == "blink")
  {
    NeoFunc = NeoTagger; // round/square 흰색은 매 loop, side는 진입 시 한 번
    lightColor(pixels_side, purple_white);
  }
  else if (device_state == "activate")
  {
    NeoFunc = NeoGaming; // round/square는 매 loop, pn532/side는 진입 시 한 번
    lightColor(pixels_pn532, purple);
    lightColor(pixels_side, purple);
    NeoGaming(); // 상단도 지금 바로 켠다 - 다음 loop까지 미루면 side보다 한 박자 늦게 켜져 보인다
  }
  else if (game_state == "activate")
  {
    // 게임은 시작됐는데 device_state가 아직 blink/activate로 안 바뀐 짧은 구간(엔진이
    // game_state를 먼저 올리는 경우). 예전엔 ReadyFunc가 남긴 빨간색이 그대로 있었다.
    NeoFunc = NeoBeforeTagger;
  }
  // 그 외 상태(player_win/lose 등)는 기존에도 LED 지정이 없어 꺼진 채 둔다.
}
// NeoFunc()는 loop()에서 매 반복마다 무조건 호출되므로, 안에서 delay()를 쓰면
// 그 시간만큼 IrSensorLoop/MicroSwLoop가 아예 안 돌아 짧은 스위치 입력을 통째로
// 놓친다. 애니메이션 없는 원색이라 페이싱 자체가 불필요 — 그냥 매번 켠다.
// A 상태 (ready) — 전체 네오픽셀 빨간색
void NeoBeforeTagger()
{
  lightColor(pixels_round, red);  // round + pn532
  lightColor(pixels_side, red);
  lightColor(pixels_square, red); // square + square2
}

void NeoTagger()
{
  // ClearRound()는 버퍼만 지우고 show()를 안 불러서 이전 색(ready의 빨간색)이
  // 화면에 그대로 남아있었다 - 색을 명시적으로 켜서 실제로 반영되게 한다.
  // blink(술래 활성화) 상태는 side와 같은 연보라로 통일한다. (예전엔 상단만 흰색이었는데
  // 흰색은 3채널을 다 켜서 같은 밝기 설정에서도 보라보다 전류를 1.5배 쓴다.)
  lightColor(pixels_round, purple_white);  // round + pn532
  lightColor(pixels_square, purple_white); // square + square2
}

void NeoTaggerTag()
{
  static int tag_neo = 0;

  ClearRound();
  pixels_side.clear();

  lightColor(pixels_round, purple, tag_neo);

  if (++tag_neo > NUMPIXELS_ROUND)
  {
    tag_neo = 0;

    ClearRound();
    pixels_side.clear();
    ClearSquare();
  }
}

void NeoAfterTagger()
{
  lightColor(pixels_round, purple);
  // lightColor(pixels_side, purple);
  lightColor(pixels_square, purple);
}

// activate 기본 화면: round/square(1,2)를 side/pn532와 같은 보라색으로 매 loop 유지한다.
// (예전엔 taken_chip/max_chip 비율로 연한 보라→진한 보라로 밝아지는 게이지였는데, 바쳐질수록
// 상단만 밝아지는 연출을 빼고 모든 스트립을 항상 같은 밝기로 두기로 했다. 칩 확인 연출
// NeoChipBlinkActivate가 끝나면 여기로 돌아와 round/square를 다시 채운다.)
void NeoGaming()
{
  // pixels_round는 lightColor()를 거치면 pn532까지 매번 덮어써버린다(pn532는
  // DataChange()에서 activate 진입 시 한 번만 고정해두는 것으로 바꿨음) — 그래서
  // 여기서는 round만 직접 갱신하고 pn532는 건드리지 않는다.
  pixels_round.fill(pixels_round.Color(purple[0], purple[1], purple[2]));
  NeoShow(pixels_round);
  lightColor(pixels_square, purple); // square + square2
}

// blink: 생명칩 태그 시 round + square(1,2)가 비율 맞춰 함께 차오르는 게이지.
// delay() 대신 NeoFunc()가 매 loop마다 한 칸씩만 진행시켜 논블로킹으로 동작한다.
void NeoChipGaugeBlink()
{
  int &step = chip_gauge_step;
  unsigned long &next_ms = chip_gauge_next_ms;

  if (millis() < next_ms) return;
  next_ms = millis() + 50;

  if (step == 0)
  {
    pixels_round.clear();
    pixels_square.clear();
    pixels_square2.clear();
  }

  lightColor(pixels_round, purple, step);
  lightColor(pixels_square, purple, step * NUMPIXELS_SQUARE / NUMPIXELS_ROUND);

  if (++step > NUMPIXELS_ROUND)
  {
    step = 0;
    pixels_round.clear();
    NeoShow(pixels_round);
    pixels_square.clear();
    NeoShow(pixels_square);
    pixels_square2.clear();
    NeoShow(pixels_square2);
    NeoFunc = NeoTagger; // blink 상태의 기본 연출로 복귀
  }
}

// activate: 생명칩 태그(+1) 시 round + square(1,2)가 보라색으로 깜빡임.
// pn532(태그 지점)는 이 연출과 무관하게 계속 보라색으로 둔다.
void NeoChipBlinkActivate()
{
  bool &on = chip_blink_on;
  int &blink_count = chip_blink_count;
  unsigned long &next_ms = chip_blink_next_ms;

  if (millis() < next_ms) return;
  next_ms = millis() + 150;

  if (on)
  {
    lightColor(pixels_round, purple);
    lightColor(pixels_square, purple);
  }
  else
  {
    // pixels_round/pixels_square를 직접 clear해서(ClearRound/ClearSquare 안 씀)
    // pn532는 건드리지 않고 보라색 그대로 유지한다.
    pixels_round.clear();
    NeoShow(pixels_round);
    pixels_square.clear();
    NeoShow(pixels_square);
    pixels_square2.clear();
    NeoShow(pixels_square2);
  }
  on = !on;

  if (++blink_count >= 6) // on/off 3회
  {
    blink_count = 0;
    on = false;
    NeoFunc = NeoGaming; // activate 기본 연출로 복귀 (pn532도 다시 보라색으로 정리)
  }
}

// void NeoTakenChip()
// {
//   static int chip_neo = 0;

//   if(chip_neo == 0){
//     pixels_side.clear();
//     pixels_square.clear();
//     pixels_round.clear();
//   }

//   pixels_round.lightColor(purple, chip_neo);

//   if(++chip_neo > NUMPIXELS_ROUND){
//     chip_neo = 0;

//     pixels_round.clear();
//     pixels_side.clear();
//     pixels_square.clear();
//   }
// }

void NeoWin()
{
  lightRgb(pixels_round, 0, 0, 255);
  lightRgb(pixels_side, 0, 0, 255);
  lightRgb(pixels_square, 0, 0, 255);
}

void NeoLose()
{
  lightColor(pixels_round, red);
  lightColor(pixels_side, red);
  lightColor(pixels_square, red);
}

//******************************************* Solenoid *******************************************
// 모스펫(IRLZ44N)으로 구동되는 솔레노이드. HIGH = 통전(ON), LOW = 차단(OFF).
// 부팅 시 한 번 배출하고, 이후 IR 칩 감지 + 마이크로스위치 조건에서 2초 통전한다.
void SolenoidInit()
{
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);
}

void SolenoidOn()
{
  digitalWrite(SOLENOID_PIN, HIGH);
}

void SolenoidOff()
{
  digitalWrite(SOLENOID_PIN, LOW);
}

void SolenoidPulse()
{
  // 통전 중에는 센서/네트워크 처리를 멈추고, 지연이 끝나면 직접 출력을 끈다.
  BlackBoxAltar("altar.solenoid.on", SOLENOID_PULSE_MS, 0, 0, 0);
  SolenoidOn();
  delay(SOLENOID_PULSE_MS);
  SolenoidOff();
  BlackBoxAltar("altar.solenoid.off", 0, 0, 0, 0);
}

//****************************************** IR Sensor *******************************************
// 생명칩이 투입구를 통과하면 IR_SENSOR가 LOW로 감지됨. ir_chip_pending을 세우고,
// 이미 태그가 확인된 상태(tag_active)면 태그+칩이 (이 순서로) 동시에 확인된
// 것이므로 OnTagChipConfirmed()로 확인 애니메이션만 트리거한다(서버 반영은
// MicroSwLoop가 크랭크 입력을 확인하고 개방하기 전에 RunAltarSuccess가 요청).
// 반대 순서(칩이 먼저 온 경우)는 CardChecking() 쪽에서 태그가 뒤늦게 확인될 때 처리한다.
void IrSensorInit()
{
  pinMode(IR_SENSOR_PIN, INPUT);
}

// "LOW가 N ms 유지돼야 인정" 방식은 접촉이 debounce 시간보다 짧으면 통째로 씹힌다.
// 대신 엣지를 즉시 인정하고, 그 뒤 일정 시간만 재감지를 무시(락아웃)해 채터링만 거른다.
void IrSensorLoop()
{
  static bool stable = false;
  static unsigned long lockout_until_ms = 0;

  if (millis() < lockout_until_ms) return;

  bool raw = (digitalRead(IR_SENSOR_PIN) == LOW);
  if (raw != stable)
  {
    stable = raw;
    lockout_until_ms = millis() + IR_SENSOR_DEBOUNCE_MS;
    if (!stable)
    {
      // 센서가 다시 HIGH로 돌아옴 - 이게 안 찍히면 칩이 센서 앞에 걸려 다음 칩을 못 본다.
      BlackBoxAltar("altar.ir.clear", ir_chip_pending, tag_active, 0, 0);
    }
    if (stable)
    {
      Serial.println("[IrSensor] Chip detected");
      BlackBoxAltar("altar.ir.chip", tag_active, ir_chip_pending, 0, 0);
      ir_chip_pending = true; // 솔레노이드 개방은 MicroSwLoop가 담당
      ir_chip_confirm_animated = false; // 새 칩 세션 시작

      if (tag_active)
      {
        ir_chip_confirm_animated = true;
        OnTagChipConfirmed();
        // tag_active는 여기서 false로 되돌리지 않는다 - 태그는 여전히 리더에
        // 붙어있고, 되돌리면 다음 폴링 때 재태그로 오인해 애니메이션이 반복된다.
      }
      else
      {
        Serial.println("[IrSensor] No active tag yet - solenoid stays closed unless a tag catches up before the crank click");
      }
    }
  }
}

//****************************************** Micro Switch *****************************************
// 회전 메커니즘이 한바퀴 돌면 딸깍 눌림 (외부 10K 풀업 → 평소 HIGH, 눌리면 LOW).
// IR센서로 칩이 들어온 걸 이미 확인했을 때만(빈 회전 제외) 처리하되, 솔레노이드는
// "지금 이 순간에도" 태그가 리더에 붙어있을 때(tag_active, blink/activate 무관)만
// 실제로 연다 - 태그 없이 칩만 넣고 돌리거나, 태그+칩이 한 번 확인된 뒤 태그를
// 떼고서 돌리는 경우엔 열리지 않는다(태그가 이미 없으므로 tag_active가 false로
// 리셋돼있음 - RfidLoop 참고). taken_chip+1 요청(RunAltarSuccess)은
// setting_state가 아닐 때 개방 전에 보낸다.
void MicroSwInit()
{
  pinMode(MICRO_SW_PIN, INPUT);
}

void MicroSwLoop()
{
  static bool stable = false;
  static unsigned long lockout_until_ms = 0;

  if (millis() < lockout_until_ms) return;

  bool raw = (digitalRead(MICRO_SW_PIN) == LOW); // LOW = 눌림
  if (raw != stable)
  {
    stable = raw;
    lockout_until_ms = millis() + MICRO_SW_DEBOUNCE_MS;
    if (!stable)
    {
      // 스위치가 떼어짐 - 눌린 채 남으면 다음 회전의 클릭 엣지가 생기지 않는다.
      BlackBoxAltar("altar.sw.release", ir_chip_pending, tag_active, 0, 0);
    }
    if (stable)
    {
      Serial.println("[MicroSw] Click detected");

      // setting 상태에서는 태그 확인이 애초에 불가능하므로(CardChecking의 tag_active는
      // game_state=="activate"일 때만 세워짐), 역할 태그 없이 그냥 IR센서가 칩을
      // 감지한 상태(ir_chip_pending)라면 마이크로스위치가 눌릴 때 바로 배출한다
      // (스태프가 세팅 중 넣어본 생명칩을 태그 없이 다시 꺼낼 수 있어야 함).
      bool setting_state = (String)(const char *)my["game_state"] == "setting";

      // 무시되는 클릭도 전부 남긴다: 칩 감지 여부, 태그 유지 여부, setting 여부.
      BlackBoxAltar("altar.sw.click", ir_chip_pending, tag_active, setting_state, 0);

      if (ir_chip_pending)
      {
        if (tag_active || setting_state)
        {
          Serial.println(setting_state
              ? "[MicroSw] Setting mode - chip present, opening without chip count"
              : "[MicroSw] Tag still present + chip confirmed -> request count, then open");
          // 조건을 충족한 입력만 소비한다 - 태그가 늦게 도착해서
          // 아직 확인 안 된 채로 클릭이 먼저 지나가도(빈 회전 아님, 그냥 아직
          // 대기 중) ir_chip_pending을 꺼버리면 나중에 태그가 와도 이미 늦어버림.
          ir_chip_pending = false;
          ir_chip_confirm_animated = false;

          // 크랭크가 눌린 순간 LED를 전부 끈다 - 서버 전송과 2초 통전(delay) 동안 암전.
          // 진행 중이던 확인 연출도 여기서 끝낸다(얼어붙은 채 이어지지 않게).
          NeoSceneOff();

          // HTTP 요청이 끝난 다음 개방한다. setting 배출은 남은 태그 상태와
          // 무관하게 집계하지 않는다. Send() 반환은 서버 반영 성공의 증거가 아니다.
          if (!setting_state && tag_active)
          {
            RunAltarSuccess();
          }
          SolenoidPulse();

          // 통전이 끝나면 현재 상태의 기본 화면으로 복귀.
          NeoSceneRestore();
        }
        else
        {
          Serial.println("[MicroSw] Chip present but tag not currently on reader - keep waiting");
        }
      }
    }
  }
}
