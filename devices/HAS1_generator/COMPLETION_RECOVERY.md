# 발전기 조건부 완료 복구

## 배포 순서

[서버 PR #87](https://github.com/Fuzzyline-HAS2/fuzzyline-core/pull/87)의
`CompleteGenerator`와 정수 `completion_protocol: 1`은 선택이다. 서버와 펌웨어는 어느
쪽을 먼저 배포해도 된다. 서버에 지원 표시가 없으면 완료는 v37과 같은 구형
`Send device_state` 1회로 보낸다. #87을 배포할 때는 Apache `has2.php`와 PHP cURL
활성화, 409 응답 전달을 확인한다. 장치는 다음 완료부터 자동으로 새 경로를 쓴다.

현재 변경은 PR용 소스이며 OTA 배포가 아니다. 펌웨어 버전 증가·정식 서명·배포는
기존 배포 워크플로에서 별도로 수행한다. 공용 라이브러리, 파티션, OTA 경로,
MQTT 설정은 변경하지 않는다.

## 완료 경로 선택

서버 지원 여부는 배선 snapshot(`GetGeneratorWires`/`SetGeneratorWires` 응답)으로만
판단한다. ReceiveMine에는 이 표시가 없다. HTTP 200, JSON 파싱, `success:true`,
epoch/revision/device_state가 모두 맞는 snapshot만 판단 근거가 된다. 구형 gateway는
Node 오류를 200 빈 본문으로 바꾸므로, 빈 본문이나 읽기 실패는 "모름"으로 남는다.
모르는 동안은 완료를 보내지 않고 5초마다 읽기 전용 `GetGeneratorWires`를 보낸다.
이후 모든 배선 응답이 값을 갱신한다.

완료(배선 완료·스타터 완료)마다 그 시점의 값으로 경로를 고른다.

- `completion_protocol: 1`이고 대시보드 우회가 아니면 아래의 조건부 `CompleteGenerator`.
- 표시가 없으면 구형 `Send device_state` 1회. 저장 기록(NVS)을 쓰지 않는다. 확인은
  `ReceiveMine`으로 하고, 연결 단계에서 요청 바이트가 하나도 나가지 않은 실패만 최대
  3회까지 다시 보낸다. 구형 서버는 중복 Send마다 `left_generator`를 또 차감하므로
  그 밖의 불확실한 실패는 다시 보내지 않는다.
- #86 우회 사이클의 `repaired`는 서버 버전과 관계없이, 지원 여부를 모르는 동안에도
  구형 Send로 보낸다. 우회 사이클은 이 기기가 직접 완충을 확인하지 않았는데 서버가
  `battery_max`/`starter_finish`를 준 경우다. 대시보드 강제가 대표적이고, 스타터 단계 중
  재부팅, 서버 재시작(epoch 변경), 조건부 요청 대체 후 재동기화도 여기에 해당한다.
  `CompleteGenerator`는 실제 배선이 기준 이상이어야 해서 409가 난다. #87 서버의 구형
  Send는 배선 조건 없이 받아 주고 중복은 no-op으로 처리한다. 서버 문서의 "protocol 1이면
  Send 금지" 원칙에 대한 의도된 예외다.

조건부 요청이 확정되지 않은 채 같은 epoch의 읽기에서 지원 표시가 사라지면(서버 되돌림)
현재 상태를 본다. 이미 목표 상태면 확정한다. 출발 상태 그대로면 기록을 지우고 같은
완료를 구형 Send 1회로 즉시 이어 보낸다. 구형 서버에는 `CompleteGenerator` 처리기가 없어
적용됐을 수 없기 때문이다. 배선·스타터 진행은 초기화하지 않고, 손님이 카드를 뗐거나
재부팅 후 복원된 기록이어도 다시 할 필요가 없다. 이때 기록 삭제가 실패하면 중복을
피하려고 Send를 보내지 않고, 준비/설정 단계까지 완료를 막는다. 완료 응답(변경 요청)은
이 판단에 쓰지 않는다. 서버 재시작으로 epoch가 바뀌면 기존 규칙대로 재동기화한다.

## 조건부 완료 요청 흐름

1. 배선의 기존 8/10 판정과 서버 CAS 확인을 유지한다. 서버 snapshot에서 완료 API
   지원과 현재 단계·배선 조건을 확인한다.
2. 장치명·MAC·원래 epoch/revision·목표 상태·배선 개수·완료 기준을 전용 NVS에
   저장하고 새 핸들로 읽어 검증한다. 저장 실패 시 완료 HTTP 요청을 보내지 않는다.
3. `CompleteGenerator`로 `battery_max` 또는 `repaired`를 보낸다.
4. 응답이 없거나 불명확하면 `GetGeneratorWires`로 현재 상태를 조회한다. 원래 조건이
   유지되면 같은 요청을 재시도한다. 3회 소진 후 영구 중단하지 않는다.
5. 현재 완료 상태가 확인되면 기록을 지우고 진행한다. epoch/revision 또는 단계가
   달라졌으면 이전 요청을 끝내고 현재 서버 상태로 다시 동기화한다.

HTTP 200만으로 완료를 인정하지 않는다. 장치 이름, 지원 버전, typed snapshot,
상태/epoch와 응답 의미를 검사한다. 응답 유실 뒤 같은 revision 재전송은 이미 적용된
요청이라면 409가 될 수 있다. **새 revision을 붙여 이전 완료를 자동 재실행하지 않는다.**
서버 프로세스가 재시작해 epoch가 바뀌어도 이전 요청을 새 epoch로 옮기지 않는다.

실패 후 재시도 간격은 약 2/4/8/16초로 증가하고 최대 500ms 지터를 더한다.
조회·큐 대기·입력 재확인 시간이 추가되므로 고정 복구 시간 보장은 아니다.
실제 배선이 달라지거나 불확정이면 완료 전송을 보류하고 현재 상태를 재평가한다.
이미 서버로 보낸 요청은 물리적으로 취소할 수 없으며 서버의 revision 검사가 최종
방어다. 서버 DB 적용과 외부 연출까지 정확히 한 번 수행되는 outbox는 없다.

## 재부팅과 저장 실패

기록은 `gen_complete` namespace의 `pending` 키에 137바이트 버전·CRC 형식으로
저장한다. 다른 설정·크래시 namespace나 NVS 전체를 지우지 않는다. 같은 요청을
재시도할 때는 플래시를 다시 쓰지 않는다. 삭제도 재조회로 확인한다.

재부팅 후 기록이 있으면 서버 상태를 먼저 읽는다. 기록 판정이 끝나기 전에는 배선
CAS가 원래 revision을 소비하지 않도록 배선 쓰기도 보류한다. 현재 서버 상태와
새 물리 표본을 확인한 뒤에만 원래 요청의 재시도를 허용한다.

저장소 읽기·쓰기·삭제 오류 또는 CRC/버전 오류는 새 완료 요청을 차단하고 진단을
남긴다. 손상된 기록을 임의 요청으로 복원하지 않는다. 준비/설정 상태에서 새 배선
snapshot으로 현재 장치와 비활성 게임 상태를 확인하면 5초 간격으로 정리를 시도한다.
삭제 검증까지 성공해야 새 완료를 허용하며 활성 게임 중에는 손상 기록을 지우지 않는다.
NVS는 전원 차단 복구를 지원하지만 차단 당시 쓰던 key/value는 유실될 수 있다.
이 구현은 commit과 새 핸들 재읽기가 모두 성공한 뒤에만 요청을 보낸다. 삭제 도중
전원이 끊겨 이전 기록이 남으면 부팅 후 서버 상태를 다시 확인한다. CRC는 손상
검출용이며 복구 수단이나 보안 서명이 아니다. 플래시 손상·전체 삭제까지 복구하는
백업은 아니며 호스트 오류 주입 시험도 실제 전원 차단 시험을 대신하지 않는다.

유효한 저장 기록이 있는 미확정 완료는 기존 Wi-Fi 오프라인 자동 재부팅을 계속
허용한다. 저장 기록이 없는 구형 Send가 현재 게임에서 전송된 뒤 미확정이면 v37처럼
재부팅을 보류한다. 재부팅 후 같은 완료를 다시 보내지 않게 하기 위해서다. OTA·실행 중 HTTP·읽지 않은 mailbox·worker fault 가드는 유지한다.
HTTP 오류만으로 재부팅하지 않으며 30초/180초 기준은
[기존 정책](OFFLINE_RECOVERY.md)을 따른다.

## 게임 동작과 관측

기여도는 기존 시점에 캡처하며 전송만 완료·배선·조회보다 낮은 우선순위를 사용한다.
기여도 API에 완료의 자동 재전송 정책을 적용하지 않는다. 폴링은 기존의 최대 3개
처리 후/4초 대기 우선 정책을 유지한다. 네트워크 대기는 단일 worker가 맡는다.

새 서버 API는 현재 배선 개수가 완료 기준 이상이어야 한다. 관리자가 `battery_max`만
강제로 지정해 배선 조건을 우회한 사이클은 그래서 위의 구형 Send로 수리 완료를 보낸다.
이 흐름은 시험 기기에서 직원 조작과 함께 확인한다.

`[NET]` 로그에서 `server completion_protocol=`, `completion target=... mode=legacy|conditional
protocol= bypass=`, 지원 표시 대기, 구형 경로 전환, 저장/복원/삭제 실패, 완료 목표·대기
시간·시도 횟수를 확인한다. 기존 BlackBox HTTP 기록과 서버·PHP 로그를 함께 대조한다. 공용 transport의
2000ms 예산은 소켓 정리까지 항상 보장하는 상한이 아니며, 실제 드라이버 정지는
worker 워치독이 담당한다.

## 검증과 현장 확인

```sh
python3 devices/HAS1_generator/tests/run_completion_store_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_network_tests.py
ARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src python3 devices/HAS1_generator/tests/run_wire_tests.py
```

시험 환경에서 요청 미전달 반복 후 복귀, 적용 후 응답 유실, 서버 재시작, 배선 제거,
직원 초기화, 다음 게임 전환을 확인한다. 저장 직전/직후·서버 적용 직후·기록 삭제
도중 전원 차단도 실제 기기에서 확인해야 한다. 여러 장치 동시 복귀, OTA 중 보류,
RFID/입력 반응성과 heap/worker stack 여유도 현장 검증 대상이다.

롤백할 때는 게임을 멈추고 서버 상태와 미확정 요청을 정리한다. 구형 펌웨어가
새 pending 기록을 해석할 수 있다고 가정하지 않으며 NVS 전체 삭제로 정리하지 않는다.

NVS 근거: [ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/storage/nvs_flash.html).
