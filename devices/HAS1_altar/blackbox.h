#pragma once
#include <stddef.h>
#include <stdint.h>
#include <Adafruit_NeoPixel.h>
#include <HAS1BlackBox.h>

// 제단 블랙박스(HAS1BlackBox) 연결부 - 구현은 blackbox.ino, 기록 형식은 BLACKBOX.md.
// 기본 인자를 쓰지 않는다: Arduino가 .ino 함수의 프로토타입을 자동 생성하므로
// 선언마다 인자 목록이 같아야 한다.
void BlackBoxBegin();
void BlackBoxLoop();
// 태그/칩/크랭크/집계/솔레노이드 흐름 기록. label은 27자 이하 ASCII 리터럴.
void BlackBoxAltar(const char *label, int32_t a, int32_t b, int32_t c, int32_t d);
// 서버 상태 전환 기록. kind 0 = game_state, 1 = device_state.
void BlackBoxAltarState(int32_t kind, const char *state);
void BlackBoxOtaBegin();
void BlackBoxOtaEnd();
int BlackBoxDiagnosticsText(char *out, size_t cap);

// 블랙박스 flash 쓰기와 LED 전송이 겹치지 않게 하는 show() 래퍼.
void NeoShow(Adafruit_NeoPixel &pixels);
void NeoRetryPending();
void NeoFlushPending(uint32_t waitMs);
