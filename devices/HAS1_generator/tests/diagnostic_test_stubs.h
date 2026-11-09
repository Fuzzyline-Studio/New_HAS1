#pragma once
#include "generator_diagnostics.h"
#ifndef DIAGNOSTIC_TEST_CHECK
#define DIAGNOSTIC_TEST_CHECK do {} while (0)
#endif
static unsigned diagnosticTestBegins=0,diagnosticTestEnds=0;
static int32_t diagnosticTestLastCode=0;
// Behavior regressions keep the logger inert; run_diagnostic_tests.py executes
// the production logger itself. Wire harness additionally rejects calls in locks.
inline void DiagnosticPrintf(const char *, ...) { DIAGNOSTIC_TEST_CHECK; }
inline void DiagnosticPoint(uint16_t,int32_t,uint32_t) { DIAGNOSTIC_TEST_CHECK; }
static unsigned blackBoxTestTraces[GeneratorTrace::Count]={};
inline void BlackBoxTrace(uint8_t kind,int32_t,int32_t,int32_t) {
  DIAGNOSTIC_TEST_CHECK; if (kind<GeneratorTrace::Count) ++blackBoxTestTraces[kind];
}
inline DiagnosticScope::DiagnosticScope(uint16_t event,uint32_t detail)
    : event_(event),id_(0),started_(0),detail_(detail),code_(0),verbose_(false) { DIAGNOSTIC_TEST_CHECK; ++diagnosticTestBegins; }
inline DiagnosticScope::~DiagnosticScope() {
  DIAGNOSTIC_TEST_CHECK;
  ++diagnosticTestEnds; diagnosticTestLastCode=code_;
  (void)event_; (void)id_; (void)started_; (void)detail_; (void)code_; (void)verbose_;
}
inline void DiagnosticScope::phase(const char *,int32_t,uint32_t) { DIAGNOSTIC_TEST_CHECK; }
