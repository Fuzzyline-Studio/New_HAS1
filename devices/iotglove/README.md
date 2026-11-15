# IoT 글러브

TTGO T1과 Beetle ESP32-C3용 1호점 The Origin 펌웨어. Nextion 없이 칩·발각 버튼·4칸 NeoPixel·진동·BLE 위치·배터리를 사용한다. **G1P1~G1P8 총 8대, 16개 보드의 초기 설치와 v3 OTA·새 부팅·Telnet 검증을 완료**했다. 장치별 실측과 재시도 이력은 [설치 현황](docs/ROLLOUT.md)과 [검증 기록](docs/VALIDATION.md)을 따른다. 게임 전체·BLE 위치·배터리 보정은 후속 검증으로 남긴다.

## 파일과 보드

통신 복구 변경의 기한·재접속 정책·진단과 실기 확인 항목은 [NETWORK_RECOVERY.md](docs/NETWORK_RECOVERY.md)를 따른다. 이 소스 변경의 검증과 기존 설치 이력은 별도로 관리한다.

기존 장치의 `library_and_pin.h`, `sensor`, `game_state`, `wifi`, `telnet` 역할 구분을 따른다. 주 `.ino`에는 버전과 진입점을 두고 기능은 `.cpp`와 명시적 헤더로 분리한다.

| 글러브 파일 | 역할 | 기존 장치에서 대응하는 구성 |
| --- | --- | --- |
| `iotglove.ino`, `iotglove.h`, `iotglove.cpp` | TTGO 진입점·초기화·출력 루프, UART와 순차 OTA·리셋 조정 | 장치명 `.ino` / `.h` |
| `library_and_pin.h` | 핀·배터리 보정·타임아웃·빌드 설정 | `library_and_pin.h` |
| `sensor.h`, `sensor.cpp` | GPIO26/27 입력 설정·첫 샘플·디바운스·칩/버튼 이벤트·진단 조회 | `sensor.ino` |
| `game_state.h`, `game_state.cpp` | 본게임·훈련 상태 전이와 카운트·타이머 규칙, 호스트 테스트 공유 | `game_state.ino` |
| `wifi_client.h`, `wifi.cpp` | first_store worker, 서버 스냅샷·순차 전송·응답 확인 | `wifi.ino` |
| `telnet.h`, `telnet.cpp`, `telnet_policy.h` | USB/Telnet 콘솔과 제한된 명령·로그 처리 | `telnet.h` / `telnet.ino` |
| `feedback.h`, `feedback_config.h` | 비차단 LED·진동 출력과 상태별 진동 설정 | `neopixel.ino`, `vibration_motor.ino`의 출력 역할 |
| `battery.h` | 배터리 샘플 평균·범위 검사; ADC 읽기와 보고는 `iotglove.cpp` | 장치 루프의 배터리 측정 역할 |
| `chip_report.h`, `network_policy.h`, `state_policy.h`, `peer_state.h`, `ota_request.h`, `link_diagnostics.h` | 서버 상태·UART peer·OTA 요청·진단의 순수 정책과 상태 보관 | 각 기능에서 사용하는 보조 헤더 |
| [iotglove_beetle](iotglove_beetle/README.md) | 별도 C3 스케치, BLE 위치·GPIO 리셋/WDT·OTA | 독립된 장치 폴더 |
| [공통 UART 라이브러리](../../libraries/IoTGloveProtocol/README.md) | 버전 프레임·파서·위치 필터·리셋 상태 처리 | 두 보드 공용 Arduino 라이브러리 |
| [빌드 방법](docs/BUILD.md), [검증 기록](docs/VALIDATION.md) | compile-only 검증, Actions/Release, 실기기 절차 | — |

서버 헤더는 ESP32 공식 `WiFi.h`와 대소문자를 구분하지 않는 파일시스템에서도 충돌하지 않도록 `wifi_client.h`로 명명했다. 각 `.cpp`는 따로 컴파일하고 필요한 선언을 헤더로 포함한다. 기능 파일을 `.ino` 탭으로 합치거나 `.cpp`를 직접 include하지 않는다. 디렉터리·주 스케치·Release 태그는 그대로 유지한다.

## 배선

| 기능 | TTGO | Beetle |
| --- | --- | --- |
| UART TTGO → Beetle | GPIO32 TX | GPIO6 RX |
| UART Beetle → TTGO | GPIO36 RX | GPIO5 TX |
| 리셋 요청 | GPIO33 출력 (v23까지는 GPIO12) | GPIO1 입력 (회로도 심벌의 물리 핀 4, 라벨 1) |
| NeoPixel 4개 | GPIO25 | — |
| 생명칩, LOW=장착 | GPIO26 | — |
| 발각 버튼, LOW=눌림 | GPIO27 | — |
| 진동, LEDC PWM 20kHz 8비트, 듀티 0=OFF | GPIO13 (모스펫 게이트, 모터에 플라이백 다이오드 필요) | — |

공통 GND와 실제 구동 회로를 확인한다. 리셋은 LOW 대기 → HIGH 150ms → LOW이며 요청 간 30초 간격을 둔다. Beetle은 부팅 후 LOW를 확인해야 재무장하고, 유효 요청 뒤 자기 watchdog feed를 중단해 재부팅한다. v24부터 리셋 요청선은 GPIO33이다. GPIO12는 MTDI 부팅 strap 핀이라 리셋 펄스 중 TTGO가 리셋되면 flash 전압 설정이 틀어질 수 있어 옮겼다. GPIO33은 strap 핀이 아니고 부팅 직후 내부 pull이 없어 펌웨어가 LOW로 잡기 전까지 플로팅이지만, Beetle 쪽 pull-down과 "LOW 50ms 뒤 HIGH 20ms" 게이트 때문에 오동작 리셋은 나지 않는다. 펌웨어와 배선이 다른 글러브(v24 이상 + GPIO12 배선, 또는 v23 이하 + GPIO33 배선)는 벽돌이 되지는 않지만 원격 Beetle 리셋만 조용히 동작하지 않으므로, 글러브별 재배선 여부를 버전 경계로 관리한다.

2026-09-17 사용자 회로도 확인으로 Beetle 리셋 입력을 GPIO3에서 **GPIO1**로 정정했다. 기존 GPIO3 이미지에서는 요청을 감지하지 못했으나, 수정본을 두 보드에 USB 설치한 뒤 `b` 펄스 후 새 boot ID와 일치 PING/HELLO 응답으로 Beetle 재부팅·UART 복구를 확인했다. UART에서 하드웨어 reset cause는 직접 읽지 않았다. 설치 이력과 실측 근거는 [검증 기록](docs/VALIDATION.md)에 구분한다.

## 본게임과 훈련

서버 기반 Academy 생존자·술래는 본게임과 동일하게 서버 `brightness`를 적용한다. 1~100을 1~255로 변환하며, 누락·범위 오류는 기본값 50을 사용한다. 서버 없는 독립 훈련 빌드만 밝기 255를 유지한다.

기본 빌드는 Origin이며 서버의 `game_state=academy`로 재컴파일 없이 훈련소를 선택한다. 이때 `device_state=player`는 칩 제거 시 파랑 1칸, 3/6/9초에 2/3/4칸, 조기 재장착은 9초 대기, 발각은 1칸부터 재시작한다. `device_state=tagger`는 입력·진동 없이 보라 4칸을 계속 켠다. 기존 `IOTGLOVE_TRAINING=1` 독립 훈련 빌드도 검증/서버 없는 시연 용도로 유지한다. 두 훈련 모드 모두 칩 없는 시작은 유령이다.

서버 연동 모드는 등록 MAC으로 서버에서 G1/G2 본게임 또는 G9 훈련소 참가자를 받는다. G9도 서버 상태를 수락하고 물리 칩 상태를 보고하며, 서버의 훈련소 역할·권한 규칙을 따른다. 서버 없이 동작하는 독립 훈련 모드와는 별개다. 펌웨어는 물리 칩 상태만 보고하고 포획·재장착 취소·소생에 따른 역할 전환은 서버가 처리한다. 서버는 활성 게임과 실제 술래의 `device_state=activate` 등을 확인한 뒤 본게임 포획을 허용한다. 활성 생존자 표시는 서버 역할을 기준으로 한다. `role=player`인 동안 칩 제거·재장착에도 초록 4칸을 유지하고, 서버가 `role=ghost`를 확인한 뒤 유령 표시로 바뀐다. 설정·준비·`photo`·종료의 표시 우선순위를 적용하며, 서버 동기화를 잃으면 마지막으로 확인한 색을 유지한다.

- 유령대기는 봉헌 전·미개방 상태에서 칩 재장착으로 복귀한다. 확정 유령은 `is_sacrificed=1`, `revival_count=4`, `is_open=1`, 실제 칩 장착을 모두 만족해야 복귀한다.
- Origin 소생 카운트·쿨타임은 서버가 계산하고 글러브는 서버 값만큼 파랑 칸을 표시한다. 4칸 표시를 위해 별도 로컬 충전 타이머를 돌리거나 카운트를 올리지 않는다. Academy·독립 훈련의 로컬 소생 타이머는 기존 규칙을 유지한다. 발각 버튼은 발견 진동(150/100/150ms)이 끝난 뒤 서버 최솟값 1을 보고하며, 그동안 worker는 새 요청 회차를 시작하지 않는다(이미 진행 중인 요청은 끝까지 간다).
- 재부팅·재동기화는 서버 카운트를 그대로 표시한다. 펌웨어가 칩 상태를 다시 보고하면 서버가 복귀 조건을 검사한다. 이후 카운트·봉헌·개방 값이 바뀌는 경우에도 서버가 재검사한다.
- 미확정 버튼 카운트(1)는 서버 재조회로 확인한 뒤 이후 서버 충전 값을 따른다.
- 첫 입력과 칩 변화는 모든 `game_state`/`device_state`에서 `SetGloveChip`으로 보고한다. 기존 `life_chip` 필드에 장착 `1` / 미장착 `0`을 설정하고, 같은 장치의 `ReceiveMine`에서 값과 `chip_report_ready=1`을 확인한다. 실패는 최소 5초 뒤 최신 상태로 재시도한다. 단절 중 과거 탈착을 재생하지 않으며 재접속·서버 재시작 후 현재 상태를 동기화한다. 독립 훈련은 서버에 보고하지 않는다.

TTGO와 Beetle OTA는 `badland_shoot`에 직접 연결한다. `first_store`의 기존 해당 AP 자격증명을 사용하며, 저장된 다른 AP나 후보 AP로 우회하지 않는다. 연결 실패 시 보드를 재부팅하지 않고 기존 재시도/OTA 실패 절차를 따른다. `badland`는 서버 테마 이름이며 SSID와 구분한다.

서버 통신은 전용 worker에서 수행한다. 1초마다 `Loop` 한 번을 보내고, 변경 flag·강제 재조회·유효 스냅샷 없음·안전 주기(유령을 포함한 모든 역할 5초)일 때만 `ReceiveMine` 전체를 읽는다. 조회 실패 뒤에는 다음 `Loop` 회차에 전체 읽기를 다시 시도한다. 위치는 바뀌면 즉시, 그 외에는 5초마다 보낸다(서버가 위치 수신 때만 vibe와 술래 음향을 다시 계산하기 때문). 첫 유효 응답 전, 자기 상태 조회 실패/무효 응답, Wi-Fi 단절, 15초 이상 오래된 상태에서는 소생 카운트 쓰기를 멈춘다. 이미 유효한 상태를 받은 뒤 동기화를 잃은 경우 적색 경고로 바꾸지 않고 마지막 역할·장치 상태의 색을 유지한다. 물리 칩 보고는 게임 상태 해석과 분리하며, 등록 MAC에 대응하는 유효한 G1/G2/G9 장치명을 새로 확인해야 전송한다. 연결 복구와 역할 전환 시 이전 카운트 큐를 재생하지 않는다. 서버에 실제 게임 epoch가 없어 단절 중 게임이 완전히 바뀌었다가 같은 상태로 돌아오는 경우의 식별은 서버 보완이 필요하다.

이 폴링 정책은 [서버 PR #92](https://github.com/Fuzzyline-Studio/fuzzyline-core/pull/92)의 자동 충전 알림 변경을 운영 서버에 먼저 배포하는 것이 전제다. 서버가 소생 카운트를 실제로 올릴 때 해당 글러브의 `shift_machine=2`를 설정하면 다음 `Loop` 회차에서 전체 읽기를 한다. 5초 안전 조회는 알림 누락·응답 유실을 보완한다. PR 생성이나 검토 완료는 운영 배포를 뜻하지 않으며, 변경이 없는 서버에서는 자동 충전 표시가 5초 안전 조회까지 늦어질 수 있다.

새 절대값 API를 포함한 `fuzzyline-core` 서버를 먼저 적용해야 한다. 기존 `Send&column=life_chip`은 다른 장치 호환을 위해 증감 동작을 유지하며, 새 글러브는 이 경로와 역할 쓰기를 사용하지 않는다. 새 API가 없는 서버에서는 칩 보고를 미확정 상태로 재시도한다. API·역할 조건과 후속 통합 범위는 [서버 계약](docs/SERVER_CONTRACT.md)을 따른다.

## 장치 상태 표시와 진동 설정

2026-09-18 사용자 확정 기준에 2026-09-25 Photo·통신 단절 정책을 반영했다. `game_state`는 게임 진행/OTA 및 서버 watchdog 제한, `device_state`는 아래 장치 표시를 구분하는 데 사용한다.

| 조건 | LED | 칩 유무 |
| --- | --- | --- |
| `device_state=setting` | 하양 | 장착 4칸 / 미장착 3칸 |
| `device_state=ready` | 빨강 | 장착 4칸 / 미장착 3칸 |
| `role=tagger`, `device_state=blink` | 보라 점멸, 500ms마다 ON/OFF | 술래 결정 전 표시 |
| `role=tagger`, `device_state=activate` | 보라 상시 점등 | 활성 술래 표시 |
| `device_state=photo`, `role=tagger` | 보라 상시 점등 | 4칸 |
| `device_state=photo`, 그 외 역할 | 초록 | 유령·neutral도 생존자로 표시, 4칸 |
| `game_state=academy`, `device_state=player` | 초록/파랑 | 독립 훈련 규칙, 1~4칸 |
| `game_state=academy`, `device_state=tagger` | 보라 | 진동 없이 4칸 상시 점등 |

준비 상태에서도 GPIO26을 계속 읽고 30ms 안정화 후 점등 수를 갱신한다. 준비 중에도 칩 상태 0/1을 보고하지만 포획·역할 변경 명령을 보내지 않는다. `photo`에서는 역할색을 표시하되 게임 쓰기·BLE 스캔·OTA 제한은 유지한다. `device_state=exploration`은 지원하지 않으며 해당 응답을 무효 처리한다. 서버 무효·단절·15초 만료는 안전 제한과 진동 취소만 적용하고 LED는 마지막 색을 유지한다. `game_state=activate`에서 장치 준비 표시를 하더라도 게임 중 OTA 및 서버 watchdog 리셋 제한은 유지한다. 수동 `b` 리셋 명령은 기존 동작을 유지한다.

진동은 [feedback_config.h](feedback_config.h)에서 상태별로 지정한다. 짧게 1회·길게 1회·짧게 2회 및 끄기를 선택할 수 있고 기본 길이는 각각 150ms, 300ms, 150ms ON → 100ms OFF → 150ms ON이다. 서버 필드를 추가하지 않으며 설정 변경은 펌웨어 릴리즈와 OTA로 반영한다.

v24부터 모터는 GPIO13의 LEDC PWM(20kHz, 8비트)으로 구동하고 [motor_drive.h](motor_drive.h)가 ON 요청의 듀티를 만든다. 근접 진동과 연속 ON(`vibe=11`)은 듀티 160에서 시작해 25ms 동안 255까지 올리는 소프트 스타트를 적용하고, 상태 전이·발각/제거 이벤트·운영자 명령(12~17) 진동은 즉시 255로 켠다. OFF는 램프 없이 즉시 0이며 OTA·리셋 펄스 중 강제 OFF, 재시작 직전 모터 OFF도 그대로다. 한 번의 ON 안에서 듀티는 내려가지 않는다(즉시 소스가 255에 도달한 뒤 근접 소스로 바뀌어도 255 유지). 램프는 틱마다 더하는 방식이 아니라 ON 시작 후 경과 시간으로 계산하므로 HTTP·flash로 루프가 멈췄다 깨어나도 바로 맞는 듀티로 간다. `out.motor`와 블랙박스·`s` 상태의 `motor`는 여전히 ON/OFF 요청이고, `motor_duty`가 마지막 PWM 듀티다. ERM 모터는 낮은 듀티에서 기동하지 않으므로 시작 듀티·램프 시간·소스별 적용 여부는 실물로 맞춘다. 모터에 플라이백 다이오드가 없는 글러브에 PWM 펌웨어를 올리면 OFF 에지마다 역기전력이 모스펫에 걸리므로 다이오드 추가와 같이 적용한다.

진동 중 LED 소등이 남는 현상을 완화하기 위해 모터 ON 중에는 500ms마다, OFF 전환 50ms 뒤에는 한 번 현재 LED 상태를 재전송한다. 색·밝기·칸 수 변경은 즉시 전송하며 정상 소등도 그대로 보존한다. 두 번 진동의 중간 OFF 구간에도 같은 규칙을 적용하고, 50ms 전에 다시 ON이 되면 이전 OFF 재전송을 취소한다. 간격은 [pixel_refresh.h](pixel_refresh.h)의 상수로 조절하며 진동 길이와 통신 루프를 지연시키지 않는다.

버튼 발각 진동 중에는 새 네트워크 요청을 잠시 보류한다. 연속 입력 시 큐의 첫 명령 시간이 아니라 실제로 마지막에 시작한 `Found` 패턴의 종료 시각까지 보류하며, 중간 OFF 구간도 포함한다. 명령은 해당 루프의 입력·진동 출력 처리 뒤에 제출한다. 이미 진행 중인 요청은 중단하지 않으며, 진동이 끝나면 대기 명령을 순서대로 처리한다.

`feedback_config::Settings`의 해당 초기값을 `Pattern::Short1`, `Long1`, `Short2`, `Off` 중 하나로 바꾼다. 길이는 `shortMs`, `longMs`, `doubleGapMs`에서 조절한다.

| 설정 | 기본 패턴 |
| --- | --- |
| `onSetting`, `onReady`, `onPhoto`, `onPlayer` | `Short1` — 짧게 1회 |
| `onGhost`, `onTaggerActive`, `onEnded` | `Long1` — 길게 1회 |
| `onTaggerBlink` | `Short2` — 짧게 2회 |
| `onRemoved` — 독립 훈련의 칩 제거 이벤트 | `Long1` — 기존 300ms |
| `onFound` — 발각 이벤트 | `Short2` — 기존 150/100/150ms |

상태 변경은 역할·장치 상태·게임 페이즈를 기준으로 감지한다. 동일 응답 폴링, LED 점멸 프레임, 칩 유무와 충전 칸 수 갱신은 추가 상태 진동을 만들지 않는다. 첫 동기화와 재접속은 현재 상태만 기준으로 저장한다. 독립 훈련의 칩 제거·발각 이벤트가 상태 변경 진동보다 우선하고, 상태 진동이 근접 진동보다 우선한다. 본게임 칩 탈착만으로는 제거 진동을 만들지 않고 서버 역할 전환 진동을 사용한다. OTA·리셋 중 진동은 취소하며 억제가 풀린 뒤 늦게 재생하지 않는다.

## 위치·배터리 설정

Beetle의 [beacon_map.h](iotglove_beetle/beacon_map.h)는 `HAS3:장치ID`의 대문자 첫 글자를 방에 매핑한다: `B` → `bamboo`, `L` → `living`, `T` → `toilet`, `S` → `sleeping`, `U` → `underground`, `H` → `hallway`. `BI1/BI2/BR1/BR2/BD1/BD2/BE/BT`는 Bamboo, `LA`는 Living Altar다. 장치 ID는 2~18자의 영문·숫자·`_`·`-`이며, 방 접두사 한 글자만으로는 장치 ID가 되지 않는다. 같은 방의 장치도 ID별로 RSSI를 따로 관리한다. 참조 `updated_IoTglove`처럼 Beetle이 1.5초 구간의 장치별 중앙값·EMA를 구하고 방별 상위 2개 신호 평균으로 위치를 판정한다. 최초 선택과 방 전환은 1.2초 유지가 필요하며, 현재 방에도 점수가 있으면 새 방이 5dB 이상 강해야 한다. 5초간 유효 비콘을 받지 못하면 위치를 무효화하고 TTGO는 서버 위치를 빈 문자열로 지운다. 판정 구조와 전송 방식의 차이는 [Beetle 설명](iotglove_beetle/README.md)을 따른다.

본게임 생존자(`player`)와 유령(`ghost`/`revival`) 모두 같은 근접 진동을 사용한다. 같은 방(`vibe=3`)은 1초당 100ms 두 번, 인접(`vibe=1`)은 2초당 100ms 한 번이다. 칩/발각 진동이 우선하며 오래된 위치로는 울리지 않는다. 실제 방 경계에서 RSSI 필터와 진동 패턴을 조정한다. 서버 `vibe` 10~17은 운영자 연출 명령이다: 10 음소거, 11 연속 ON, 12/13/14 짧은(200ms) 1~3회, 15/16/17 긴(600ms) 1~3회. 12~17은 값이 바뀌는 순간 1회만 울리고 상태·역할과 무관하게 동작한다(`docs/SERVER_CONTRACT.md`의 "vibe 연출 명령" 절 참고).

배터리는 `analogReadMilliVolts()`의 보정 ADC 값을 20ms마다 모아 16개를 평균한다. 다음 값을 [library_and_pin.h](library_and_pin.h)에 실측해 넣어야 보고가 활성화된다.

- `IOTGLOVE_BATTERY_PIN`: GPIO35는 기존 코드의 후보이며 실물 배선을 확인한다.
- `IOTGLOVE_BATTERY_DIVIDER_RATIO`: 실제 분압비 `(R위 + R아래) / R아래`.
- `IOTGLOVE_BATTERY_CALIBRATION`: 멀티미터 비교 보정값.
- `IOTGLOVE_BATTERY_MIN_MV`, `IOTGLOVE_BATTERY_MAX_MV`: 배터리 구성에 맞는 측정 허용 범위.

보정 상수가 없어도 핀의 raw ADC mV는 1초마다 읽어 `s` 상태 `[battery] adc_mv=`와 링크 손실 기록에 남기므로, 멀티미터 실측값과 비교해 분압비를 정할 수 있다. 미설정·0/포화 등 무효 ADC 값·범위 이탈은 전압 보고를 생략한다. 보고는 기존 계약대로 `battery_remaining`에 **V, 소수 2자리**, 최소 60초 간격으로 수행한다. 떠 있는 ADC의 모든 단선을 소프트웨어만으로 판별할 수는 없다. 잔량 %로 변환하지 않는다. ADC 범위/보정 API 근거: [Espressif Arduino ADC](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html).

## 관리·OTA

Beetle의 초기 설치가 끝난 뒤에는 **TTGO USB Serial 115200 또는 같은 LAN의 TTGO Telnet 포트 23으로 두 보드를 진단**할 수 있다. `telnet <TTGO-IP> 23`으로 접속하면 IP·MAC·TTGO 버전·파티션·부팅 ID와 현재 상태가 출력된다. Telnet에서는 아래 한 글자 명령 뒤에 Enter를 입력한다. USB 명령은 기존처럼 한 글자로 동작한다. Telnet은 인증·암호화 없는 운영 LAN 콘솔이며 동시에 한 클라이언트만 연결한다. 훈련소 오프라인 빌드에서는 Telnet을 시작하지 않는다.

| TTGO 명령 | 확인 내용 |
| --- | --- |
| `s` 또는 `?` | IP·MAC, 서버 valid/fresh·device·phase, GPIO26/27 원시값·디바운스 입력·모델 칩 상태, 역할·device_state·동기화·life_chip·로컬/서버 소생 카운트·봉헌/개방, 마지막 LED/모터 출력과 밝기, 로그 유실량, peer known/online, 펌웨어·파티션·부팅 ID, 마지막 유효 수신/HELLO 경과시간, heartbeat의 freshness·uptime·scan·OTA busy, 위치 freshness/방, UART 송수신 누계 |
| `p` | 현재 PING 요청 ID와 **동일한 ID의 유효 HELLO**를 확인하고 RTT 출력. 자동 PING도 2초마다 같은 방식으로 추적 |
| `b` | GPIO33→GPIO1에 리셋 펄스를 보낸 뒤 최대 15초 동안 새 부팅 ID와 펄스 이후 발급한 PING의 일치 응답을 확인 |
| `u` | 두 보드 순차 OTA 요청 |

`inputs`의 GPIO 원시값은 조회 순간의 HIGH=1/LOW=0이며, 현재 칩·버튼 입력은 LOW를 장착·눌림으로 해석한다. `chip_debounced`/`button_debounced`는 30ms 안정화 후 논리값이고 `chip_model`은 게임 모델이 반영한 장착 여부다. `outputs`는 직전 렌더의 명령 캐시이며 `cache_valid=1`일 때 읽는다. RGB는 밝기 적용 전 값, `lit`은 점등 개수, `brightness8`은 0~255 밝기이며, `motor`는 마지막 GPIO13 ON/OFF 요청, `motor_duty`는 마지막 PWM 듀티(0~255)다. 실제 발광·진동을 측정한 값은 아니므로 실물 관찰과 함께 비교한다. 상태 조회는 게임 피드백이나 입력을 소비하지 않는다.

`probe.latest=matched`, `matched_fresh=1`은 현재 부팅 ID에 대해 최근 양방향 응답을 확인했다는 뜻이다. HEART, 부팅 시 ID 0 HELLO, 이전 요청의 응답, 1.5초 timeout 뒤 도착한 응답으로 이를 갱신하지 않는다. `online=1`만으로는 양방향 연결을 입증하지 않는다. `heart.fresh=0`이면 uptime/scan/busy는 과거 샘플이며, 서버와 게임 상태에 따라 정상 연결에서도 scan은 0일 수 있다. `rx`/`tx` 누계는 TTGO 부팅 이후 값이고 핀 전압 측정값은 아니다.

`b`의 기준 부팅 ID는 명령 입력 시점이 아닌 **실제 HIGH 펄스 시작 시점의 fresh HELLO**에서 고정한다. OTA나 30초 리셋 간격 때문에 펄스가 대기할 수 있다. 접수 로그와 `reset_pending`/`pulse_high`는 현재 요청 상태이며, `last_pulse_result`/`last_pulse_age_ms`는 마지막으로 실제 출력한 펄스의 결과와 경과시간이다. 새 요청이 대기 중일 때 이전 성공을 새 요청의 성공으로 해석하지 않는다. `reboot_observed`는 그 기준과 다른 부팅 ID를 펄스 이후 PING의 정확한 HELLO 응답에서 확인한 결과다. 기준이 없거나 오래됐으면 `baseline_unknown_not_confirmed`, 기한 내 증명이 없으면 `timeout_not_confirmed`로 표시한다. Beetle v1에는 `reset_reason`이 없어 원인은 unknown으로 남는다. 새 Beetle 펌웨어의 추가 `LOG` 프레임은 실제 `esp_reset_reason()` 숫자와 리셋 요청·OTA 연결/검증/결과 단계를 전달한다. TTGO는 현재 peer 부팅 ID에 맞는 순서의 로그만 받아들이고, PING마다 재전송되는 부팅 원인 스냅샷은 별도로 중복 제거한다. 새 부팅 ID만으로 WDT 원인을 추정하지 않는다.

USB와 Telnet은 독립된 고정 크기 최근 로그 버퍼를 사용한다. 느린 클라이언트의 소켓 처리는 별도 작업에서 비차단 방식으로 수행하며, 출력이 5초간 진행되지 않으면 연결을 닫는다. Telnet 수신은 작업 회차당 64바이트, 송신은 128바이트, 명령은 초당 8개로 제한한다. 상태 출력은 1초에 한 번이고 유실 바이트/거부 명령 수는 `s`로 확인한다. 오래된 버퍼 로그 뒤에 현재 접속 배너와 상태가 이어질 수 있으므로 최신 상태와 freshness를 확인한다. Wi-Fi 연결 로그는 고정된 상태 문구만 표시하며 SSID·비밀번호·HTTP 본문은 전달하지 않는다. Beetle 로그 역시 정해진 이벤트 이름과 숫자만 사용한다.

서버의 `device_state`로 최신 또는 특정 버전을 선택한다.

| `device_state` | 동작 |
| --- | --- |
| `github` | 기존 고정 릴리즈 `iotglove`, `iotglove_beetle` 확인 |
| `github@12:7` | TTGO 버전 12, Beetle 버전 7로 설치 또는 롤백 |
| `github@12` | 두 보드 모두 버전 12로 지정 (`github@12:12`와 같음) |

숫자는 각 스케치의 정수 `FIRMWARE_VER`이다. 두 보드는 독립적으로 버전을 증가시키므로 보통 `TTGO:Beetle`을 각각 지정한다. 지정 버전은 `iotglove-v12`, `iotglove_beetle-v7`처럼 보관된 릴리즈가 있어야 한다. 참조 저장소의 `github_dev/rc/prd` 채널 명령은 사용하지 않는다. 잘못된 명령이나 없는 버전을 최신 버전으로 대신 설치하지 않는다.

게임/`photo` 중에는 시작하지 않는다. Beetle의 요청 ID·새 부팅 ID·실행 버전으로 성공 또는 동일 버전 skip을 확인한 후 TTGO를 업데이트한다. 지정 OTA는 Beetle이 **요청 버전과 정확히 일치**해야 진행하며 이전 버전도 허용한다. 실패·timeout은 TTGO 업데이트를 중단한다. 진행 중 다른 버전 명령이 오면 진행 중 목표는 바꾸지 않고 다음 요청으로 보관한다.

Beetle 완료 후 TTGO 시작 전에 게임/`photo`로 전환되면 남은 TTGO 단계를 취소한다. 서버 응답을 기다리는 동안에도 작업 시작 후 5분 안에 대기를 끝내고, 완료 확인 뒤 Beetle의 버전이 바뀌거나 5초간 연결이 끊기면 해당 확인을 무효화한다. 아직 시작하지 않은 TTGO 요청은 Wi-Fi 단절 시 실패 처리한다. 이미 플래시에 쓰기 시작한 작업은 중간에 강제 종료하지 않는다.

버전 지정 경로는 보드·펌웨어 버전·파티션 버전·이미지 HMAC을 담은 `ota.txt`와 그 서명 `ota.sig`를 검증한다. 이미지가 해당 정보와 일치해야 OTA 슬롯을 확정하며, 파티션 버전이 다르면 USB 설치가 필요하다. 기존 `github` 경로는 SecureOTA의 고정 릴리즈 규약을 유지한다. 두 보드 업데이트는 순차 처리이므로 TTGO 단계에서 실패하면 Beetle만 변경된 상태일 수 있다. 원인을 해결하고 같은 버전 쌍을 다시 요청하면 이미 맞는 보드는 skip한다. 재시도는 `device_state=setting`이 읽힌 뒤 다시 원하는 명령으로 설정한다.

버전 보관은 이 기능을 배포한 이후부터 적용되며, 예전에 덮어쓴 바이너리는 복원하지 않는다. 두 보드의 최초 USB 설치와 롤백 대상 모두 이 프로토콜을 지원하는 본게임 펌웨어여야 한다. 버전 선택 UI를 서버에 추가한 것은 아니며 기존 서버 관리 기능에서 `device_state` 값을 설정한다. 자세한 배포/보관 규칙은 [빌드 문서](docs/BUILD.md)를 따른다.

서버 `watchdog=1`은 비활성 상태에서 0으로 응답 확인 후 Beetle 리셋 펄스를 보내고 TTGO도 재부팅한다. UART 단절만으로 자동 리셋하지 않는다. 정상 OTA 중 GPIO 리셋은 보류된다.

서명 키는 각 보드의 ignored `secrets.h`와 Release 서명 키를 맞춘다. 최초 두 보드는 동일 `min_spiffs` 파티션으로 USB 설치하며 **현재 펌웨어는 파티션 OTA를 실행하지 않는다**. Actions의 파티션 파일 게시는 기존 배포 규칙상 가능하지만, 글러브 파티션 변경은 USB 절차가 필요하다. 최초 설치·TTGO 복구에 이어 GPIO1 리셋 수정본을 두 보드에 USB 설치하고 원격 재부팅·UART 복구를 확인했다. 두 대상의 GitHub Actions와 Release 서명 검증을 완료했고, G1P1에서 `github@2:2`로 Beetle과 TTGO가 차례로 v2에 재부팅한 뒤 파티션 1·서버 fresh 상태·Telnet 재접속을 확인했다. 최초 업데이트 당시 TTGO v1은 Beetle `LOG`를 소비하지 않았으므로, 업데이트 진행 증거는 UART 기록에, v2의 BOOT 로그·상태·PING 수신 증거는 이후 Telnet 기록에 구분한다. 최신 실기 확인 결과는 [검증 기록](docs/VALIDATION.md)에 남긴다.

개발 현황은 [SW 개발](https://app.notion.com/p/3dd0bd3810bf81eb8bf9f947c26d1333) 한 항목에서 관리한다. 테스트로 확인한 문제는 재현 조건을 갖춘 뒤 별도 서브아이템으로 분리한다.
