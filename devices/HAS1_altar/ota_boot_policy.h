#ifndef HAS1_ALTAR_OTA_BOOT_POLICY_H
#define HAS1_ALTAR_OTA_BOOT_POLICY_H

#include <stdint.h>

// GitHub OTA 전용 부팅(BLE·블랙박스 없이 뜨는 부팅)을 언제 요청하고 언제 그만둘지 정한다.
// Arduino 의존성이 없어 호스트 테스트(tests/run_ota_boot_tests.py)로 그대로 검증한다.
//
// 상태는 RTC no-init 메모리에 둔다: 재부팅(소프트웨어·패닉·워치독)을 건너가야 하고, 전원을
// 넣은 직후의 값은 쓰레기일 수 있다. 그래서 magic으로 유효성을 보고 전원 투입 때는 지운다.
//
// 한 번의 github 지시로 OTA 전용 부팅은 최대 kMaxAttempts번이다. 그 부팅이 죽거나(패닉),
// SecureOTA가 파티션 갱신 뒤 재부팅하거나, 확인이 실패해 서버 상태가 github에 남아도
// 재부팅이 끝없이 돌지 않는다. 서버 상태가 github을 벗어나면 횟수를 지운다.
struct OtaBootState {
    uint32_t magic;
    uint32_t request;  // 1 = 다음 부팅은 OTA 전용
    uint32_t attempts; // 이번 github 지시로 OTA 전용 부팅을 시작한 횟수
};

namespace OtaBootPolicy {

static const uint32_t kMagic = 0x4f544233;
static const uint32_t kMaxAttempts = 3;

// 부팅 때 한 번. 이번 부팅이 OTA 전용이면 true.
inline bool begin(OtaBootState &state, bool power_on) {
    if (power_on || state.magic != kMagic || state.request > 1 || state.attempts > kMaxAttempts) {
        state.magic = kMagic;
        state.request = 0;
        state.attempts = 0;
    }
    if (!state.request) {
        return false;
    }
    state.request = 0;
    ++state.attempts;
    return true;
}

// 정상 부팅에서 github 지시를 봤을 때: OTA 전용 부팅으로 재시작해도 되는가.
inline bool mayRequest(const OtaBootState &state) {
    return state.attempts < kMaxAttempts;
}

inline void request(OtaBootState &state) {
    state.request = 1;
}

// 서버 상태가 github이 아니다 - 다음 github 지시는 처음부터 다시 센다.
inline void leftGithub(OtaBootState &state) {
    state.request = 0;
    state.attempts = 0;
}

} // namespace OtaBootPolicy

#endif
