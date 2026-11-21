# 제단 블랙박스

제단(`HAS1_altar`, 본게임 LA · 훈련소 AA)이 태그·칩·크랭크·집계 요청·솔레노이드 흐름을
`libraries/HAS1BlackBox`의 flash 링에 남기고, 게임 밖에서 MQTT로 수집 PC
(`tools/windows-blackbox-logger`)에 올린다. 게임 HTTP(`has2wifi`), 워치독, OTA 절차,
파티션 표는 바꾸지 않는다. 라이브러리의 동작과 한계는
[라이브러리 README](../../libraries/HAS1BlackBox/README.md)를 따른다.

넣은 이유는 두 가지 사건이다. 2026-11-21 세션 1843에서 제단이 술래 태그 직후 PANIC으로
재부팅했는데 어느 줄에서 죽었는지 남지 않았고(부팅 배출이 칩을 집계 없이 내보냄), 세션
1847에서는 태그는 됐는데 집계가 안 나간 이유(칩 미감지 / 태그 이탈 / 클릭 누락)를 서버
로그만으로 가를 수 없었다.

## 설정

`library_and_pin.h`의 기본값. 정식 배포는 CI가 `secrets.h`에 broker URI와 암호를 쓴다.

| 매크로 | 기본값 | 뜻 |
|---|---|---|
| `HAS1_ALTAR_BLACKBOX_ENABLED` | `1` | `0`이면 기록/업로드 태스크를 만들지 않는다 |
| `HAS1_ALTAR_BLACKBOX_BROKER_URI` | `""` | 비어 있으면 flash에만 남긴다 |
| `HAS1_ALTAR_BLACKBOX_MQTT_USERNAME` | `""` | 비어 있으면 장치 STA MAC(대문자 12자리) |
| `HAS1_ALTAR_BLACKBOX_MQTT_PASSWORD` | `""` | 저장소에 커밋하지 않는다 |

브로커에는 제단마다 MAC 이름의 계정이 있어야 한다. 펌웨어 설정만으로는 계정이 생기지
않는다([RELEASE.md](../../docs/blackbox/RELEASE.md)). 수집기는 `HAS1_altar` 장치 토큰을
아는 버전이어야 한다(`protocol.py`).

파티션은 기존 `min_spiffs` 표(128 KiB `spiffs` + `coredump`)를 그대로 쓴다. `spiffs`에
파일이 있거나 모르는 내용이 있으면 지우지 않고 기록만 끈다(`bb_status` 5).

## 기록

값은 v[0..3]. 표의 흐름 기록은 `Recovery` 유형(7)이다. 라이브러리가 이 유형의 최근 8건을
RTC에도 복사하므로, 패닉·워치독 재부팅 직전 기록이 flash에 못 내려갔어도 다음 부팅에서
원래 부팅 ID·uptime으로 다시 기록된다(중복 가능, 전원 차단 시에는 보존되지 않는다).

| 텍스트 | 값 |
|---|---|
| `altar.tag.fetch` | 태그 조회(HTTP) 걸린 ms, 술래 여부, 제단 game_state가 activate인지 |
| `altar.tag.active` | 술래 태그 인정 시점. 그때 칩 대기(IR) 여부 |
| `altar.tag.left` | 태그 이탈로 게이트 내림. PN532 리더 정상 여부 |
| `altar.ir.chip` | 칩 감지(IR LOW). 그때 태그 유지 여부, 이미 칩 대기 중이었는지 |
| `altar.ir.clear` | IR가 HIGH로 복귀. 칩 대기 여부, 태그 유지 여부 |
| `altar.sw.click` | 크랭크 클릭(무시된 것 포함). 칩 대기, 태그 유지, setting 여부 |
| `altar.sw.release` | 스위치가 떼어짐. 칩 대기, 태그 유지 |
| `altar.count.begin` | `taken_chip +1` 전송 직전 |
| `altar.count.end` | 전송 함수가 돌아옴. 걸린 ms (서버 반영 여부가 아니다) |
| `altar.solenoid.on` | 통전 시작. 통전 길이 ms |
| `altar.solenoid.off` | 통전 끝 |
| `altar.taken.max` | 서버가 준 taken_chip, max_chip 변화 |
| `altar.rfid.reinit` | PN532 재초기화 예약 |
| `altar.rfid.recovered` | PN532 복구. 그동안 실패 횟수 |
| `ota.github.reboot` | github 지시로 OTA 전용 부팅으로 재시작. 그때 free heap, 최대 연속 블록 |
| `ota.github.skipped` | 이번 github 지시는 이미 시도해서 다시 재부팅하지 않음 |
| `altar.led.forced` | 암전 프레임을 가드 없이 보냄(깨졌을 수 있음). 스트립 수, 기다린 ms |

그 밖의 기록:

| 유형 / 텍스트 | 값 |
|---|---|
| `Boot` `device.boot` | 리셋 사유, 펌웨어 버전, 파티션 버전 |
| `State` 상태 문자열 | v[0] = 0 game_state / 1 device_state 전환 |
| `Progress` `main.loop` | 60초 동안 loop가 가장 오래 멈춘 ms, loop 횟수 |
| `Health` `memory.main` | free heap, 최소 heap, 최대 연속 블록, loop 스택 여유 |
| `WifiSummary` `wifi` | Wi-Fi 상태, RSSI (`INT32_MIN` = 모름) |
| `WifiEvent` | 연결/끊김/IP (라이브러리 `BlackBoxWifi.h`) |
| `Crash` `prior.core…` | 직전 부팅의 패닉 PC·백트레이스 (라이브러리) |

플레이어 이름·UID·서버 응답 내용은 남기지 않는다.

### 읽는 법

- **정상 봉헌**: `tag.fetch` → `tag.active` / `ir.chip` (순서 무관) → `sw.click`(1,1,0) →
  `count.begin` → `count.end` → `solenoid.on` → `solenoid.off`.
- **집계 없이 열림**: `Boot device.boot` 바로 뒤의 `solenoid.on/off`는 부팅 배출이다.
  그 앞 부팅의 마지막 기록(RTC 재기록 포함)과 `prior.core`가 죽은 위치를 알려 준다.
- **태그는 됐는데 안 열림**: `sw.click`이 없으면 클릭이 안 들어온 것(스위치·폴링 누락),
  `sw.click`(0,…)이면 칩 미감지, `sw.click`(1,0,…)이면 클릭 순간 태그 이탈.
- `main.loop`의 최대 멈춤은 블로킹 HTTP와 2초 통전을 포함한다. 클릭은 loop 폴링이라
  이 멈춤 안에서 눌렸다 떼어지면 기록 자체가 없다.

## 게임에 주는 영향

- **업로드**: `game_state`를 아직 못 받았거나 `activate`인 동안, Wi-Fi가 끊긴 동안에는
  올리지 않는다. 게임이 끝나 `setting`/`ready`가 되면 밀린 기록을 올린다. 진행 중이던
  세션이 권한 회수 뒤 끝나는 데는 시간이 걸릴 수 있다.
- **LED**: flash 쓰기가 LED 전송 중간에 끼면 프레임이 깨질 수 있어 모든 `show()`가
  `NeoShow()`를 거친다. 가드를 못 잡으면 그 스트립은 보류되고 다음 loop에서 그 시점의 픽셀
  버퍼로 다시 보낸다(loop는 기다리지 않는다). 통전 직전 암전(`NeoSceneOff`)만 최대
  100 ms 기다리고, 그래도 못 잡으면 가드 없이 보낸다.
- **기록 호출**: `log()`는 대기 없는 큐 넣기다. flash 쓰기는 별도 태스크가 한다. 큐(32건)가
  차면 버리고 개수를 센다(`bb_dropped`).
- **OTA**: 블랙박스는 메모리를 약 30 KiB 상시 쓰고, GitHub TLS는 약 45 KiB(연속 17 KiB
  블록 2개)가 필요하다. 그래서 `device_state`가 `github`이 되면 제단은 표시를 남기고
  재부팅해 **BLE와 블랙박스를 켜지 않은 OTA 전용 부팅**에서 `ota.check()`를 돌린다.
  업데이트가 없거나 실패하면 한 번 더 재부팅해 정상으로 돌아온다(재부팅마다 2초 부팅 배출이
  돈다). 서버 상태가 `github`에 남아 있어도 같은 지시로 재부팅을 반복하지 않으며,
  `github`을 벗어났다가 다시 들어와야 새로 시도한다. OTA가 5분 넘게 멈추면 타이머가
  재부팅한다.
- 블랙박스 태스크는 워치독을 먹이지 않고, 블랙박스가 재부팅을 일으키지도 않는다.

용량은 1,920건이다. 한 게임에 흐름 기록 100~150건 + 분당 4~6건 정도라 몇 게임 분량이지만
추정치다. 가득 차면 오래된 것부터 덮어쓰고 `Lost`로 범위를 남긴다.

## 확인

텔넷 접속 첫 줄 `[diagnostics]` 끝에 블랙박스 상태가 붙는다: `bb_ready`(0/1),
`bb_status`(1 = 정상, 5 = 모르는 내용이 있어 손대지 않음), `bb_partition`(spiffs 바이트),
`bb_flash_errors`, `bb_uploaded`, `bb_upload_failures`, `bb_dropped`,
`bb_core_state`(1 이번 리셋의 코어덤프, 2 예전 것, 3 없음), `bb_core_pc`. 그 앞의
`heap`/`heap_block`은 free heap과 최대 연속 블록, `ota_boot`는 OTA 전용 부팅(1) 여부다.

호스트 테스트(`tests/run_solenoid_tests.py`, `tests/run_rfid_presence_tests.py`)는 실제
`sensor.ino` 함수를 가짜 하드웨어로 돌려, 기록 요청이 집계·통전 순서를 바꾸지 않는지와
무시된 클릭도 기록되는지를 본다. flash 타이밍, LED 실물, MQTT, 실제 전원 차단은 검증하지
않는다. 배포 뒤 대기 상태의 제단 한 대가 수집 PC SQLite에 저장되는지, `memory.main`의
heap 여유, LED 깨짐 여부를 먼저 확인한다.
