# 3호점 현장 적용 순서 (태그머신 UT)

대상 PC는 3호점 게임 서버 `172.30.1.43`(HAS3-ServerPC)이고, 감시 대상은 태그머신 UT
(`2C:BC:BB:A7:C1:8C`) 한 대다. 이 문서는 실제 설치 완료를 의미하지 않는다. 게임이 없는
시간에 진행한다. 1호점과 다른 점만 적는다. 나머지는 [README.md](README.md)와
[STORE1-HANDOFF.md](STORE1-HANDOFF.md)를 따른다.

## 1호점과 다른 점

- **브로커를 새로 설치하지 않는다.** 이 PC에는 글러브 게임 통신용 Mosquitto 서비스
  (`mosquitto`, 1883, `allow_anonymous true`, 암호 파일 없음)가 이미 돌고 있다.
  [FIRST-INSTALL.md](FIRST-INSTALL.md)의 `first-install.ps1`은 1호점 전용이고 기존 브로커가
  있으면 중단하므로 실행하지 않는다. 익명 허용 브로커라 장치·수집기 모두 어떤 사용자
  이름/암호로도 접속된다. 장치 펌웨어는 암호를 비워 두고, 수집기 암호는 아무 값이나
  넣는다(인증에 쓰이지 않는다). 장치별 ACL이 없으므로 격리된 매장 LAN에서만 쓴다.
- **Omada·PHP 게이트웨이 입력은 켜지 않는다.** `gateway_udp`는 1호점 전용이다.
- **감시 규칙은 발전기 감시를 그대로 쓴다.** `telemetry.incidents.generators`에 UT를 넣고
  `role`을 `tagmachine`으로 둔다. 판단은 MAC만으로 한다.
- **무응답 기준은 45초**(`silence_seconds`)다. 태그머신은 태그 진행 중(로그인~개폐 연출)에
  서버 폴링 타이머를 멈춰서, 정상 동작에서도 MAC이 붙은 요청이 20초 넘게 비는 구간이 있다.
- **펌웨어**: `updated_tagmachine_main`의 `third_store` 브랜치, 블랙박스 포함 버전(v7 이상).
  장치 토큰은 `HAS3_tagmachine`이고 파티션 표는 그대로다(`Config::largePartition`).
  `coredump` 파티션이 없어 재부팅 카드에 패닉 백트레이스는 붙지 않는다.

## 순서

1. 모든 사용자용 Python 3.11+ 64비트를 **Program Files 아래**에 설치한다. `service.ps1`은
   LocalService가 읽을 수 있도록 그 위치만 받는다(`C:\Python314`나 사용자 폴더 설치는 거부).
2. `config.store3.example.json`을 복사해 설정을 만들고 README의 서비스 설치 절차로
   `HAS1BlackBoxCollector` 서비스를 설치한다(서비스 이름은 1호점과 같다).
3. 게임 서버(`fuzzyline-core`)의 `store/web-server/.env`에 아래를 넣고 게임 사이에
   서버를 재시작한다. 서버 문서 `docs/BLACKBOX-TELEMETRY.md`의 "다른 매장 적용" 참고.

   ```text
   HAS1_TELEMETRY_ENABLED=1
   HAS1_TELEMETRY_SITE_ID=store-3
   HAS1_TELEMETRY_SOURCE_ID=store-3-game-server
   HAS1_TELEMETRY_EXPECTED_THEME=error
   HAS1_TELEMETRY_DEVICE_TYPES=iotglove,generator,tagmachine
   HAS1_TELEMETRY_DIR=C:\ProgramData\HAS1BlackBox\server-telemetry
   ```

4. 수집기 설정에서 `server_jsonl`, `device_import`, `clock_probe`, `incidents`,
   `incidents.generators`를 켠다. `server_jsonl.source_id`는 `store-3-game-server`,
   디렉터리는 위 `HAS1_TELEMETRY_DIR`과 같아야 한다.
5. UT에 블랙박스 펌웨어를 OTA로 올리고, 텔넷(`172.30.1.181:23`) 첫 줄에서
   `bb_ready=1 bb_status=1`을 확인한다. 대기 상태에서 `bb_uploaded`가 늘고 수집 DB에
   `HAS3_tagmachine` 기록이 쌓이는지 본다.
6. Slack·Notion을 연결한다. 채널은 `#has3-device-alerts`(채널 ID는 생성 후 채움)이고, 봇을
   그 채널에 초대한다. Notion data source ID는 1호점과 같은 `HAS1 장치 이벤트` DB를 쓴다면
   STORE1-HANDOFF의 값이다. 이벤트는 `매장=store-3`, `역할=tagmachine`으로 기록된다.

   ```powershell
   .\configure-notifications.ps1 -EnableSlack -EnableNotion `
     -SlackChannelId '<#has3-device-alerts 채널 ID>' `
     -NotionDataSourceId '<HAS1 장치 이벤트 data source ID>'
   Restart-Service HAS1BlackBoxCollector
   ```

## 현장 합격 기준

- UT의 MQTT 기록이 SQLite에 저장되고 ACK를 받는다(`bb_uploaded` 증가, `bb_upload_failures` 정체).
- 서버 JSONL에 UT MAC의 `request_ingress`가 들어온다.
- 정상 게임 한 판(태그·잠금·해제 포함)에서 장애 알림이 나오지 않는다.
- 승인된 시험에서 UT 전원을 뺐다 꽂으면 Slack에 끊김→복구(재부팅 표시)가 한 카드로 온다.
- LED 깨짐, 효과음 누락, 태그 반응 지연이 이전 펌웨어보다 나빠지지 않았다.

서비스 Running이나 사전 검사 통과만으로 실제 수집·Slack 발송·Notion 저장을 검증한 것은 아니다.
