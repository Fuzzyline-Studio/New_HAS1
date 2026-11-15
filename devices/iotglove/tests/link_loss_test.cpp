#include "link_loss.h"
#include "network_recovery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace iotglove;

int main() {
  // RTC RAM starts with arbitrary content after a cold boot: no magic, no record.
  LinkLossRecord record;
  memset(&record, 0xA5, sizeof(record));
  assert(!linkLossPresent(record) && !linkLossPending(record));

  LinkLossContext context;
  context.stateKnown = true; context.phase = 5; context.role = 3; context.deviceState = 4;
  context.motor = true; context.lit = 4; context.motorAgeMs = 120;
  context.heapFree = 177000; context.batteryAdcMv = 1980; context.batteryMv = 0;
  linkLossCapture(record, 5000, 4200, 200, -71, 0, context);
  assert(linkLossPresent(record) && linkLossPending(record));
  assert(record.cause == static_cast<uint8_t>(LinkLossCause::None));
  assert(record.reason == 200 && record.rssi == -71);
  assert(record.lostAtMs == 4200 && record.noticedAtMs == 5000 && record.restartAtMs == 0);
  assert(record.stateKnown && record.phase == 5 && record.role == 3 && record.deviceState == 4);
  assert(record.motor && record.lit == 4 && record.motorAgeMs == 120);
  assert(record.heapFree == 177000 && record.batteryAdcMv == 1980 && record.batteryMv == 0);

  // A restart completes the pending record without touching the loss moment.
  LinkLossContext later = context;
  later.motor = false; later.motorAgeMs = 61000;
  linkLossComplete(record, LinkLossCause::WifiDown, 65000, 4, 1, later);
  assert(!linkLossPending(record) && linkLossPresent(record));
  assert(record.cause == static_cast<uint8_t>(LinkLossCause::WifiDown));
  assert(record.restartAtMs == 65000 && record.connectFailures == 4 && record.restartAttempt == 1);
  assert(record.lostAtMs == 4200 && record.reason == 200 && record.rssi == -71 && record.motor);
  assert(!strcmp(linkLossCauseName(static_cast<LinkLossCause>(record.cause)), "wifi_down"));

  // A worker stall with the STA still associated starts its own record.
  linkLossClear(record);
  assert(!linkLossPresent(record));
  LinkLossContext stall;
  stall.stateKnown = false; stall.motorAgeMs = kLinkLossMotorNever;
  linkLossComplete(record, LinkLossCause::WorkerStall, 90000, 0, 2, stall);
  assert(linkLossPresent(record) && !linkLossPending(record));
  assert(record.lostAtMs == 0 && record.reason == 0 && record.rssi == 0);
  assert(record.noticedAtMs == 90000 && record.restartAtMs == 90000 && record.restartAttempt == 2);
  assert(!record.stateKnown && record.motorAgeMs == kLinkLossMotorNever);
  assert(!strcmp(linkLossCauseName(static_cast<LinkLossCause>(record.cause)), "worker_stall"));
  assert(!strcmp(linkLossCauseName(LinkLossCause::None), "none"));

  // Outage clock: a link that was up gets 10s, a boot that never associated 180s,
  // both counted from the first observation of the link reading down.
  LinkOutageClock never;
  never.observe(false, 10000);
  assert(!never.wasUp() && never.limitMs() == 180000 && never.downMs(10000) == 0);
  assert(!never.tooLong(189999) && never.tooLong(190000));
  never.observe(false, 150000);  // Repeated down samples never move the start.
  assert(never.downMs(190000) == 180000);
  LinkOutageClock lost;
  lost.observe(true, 0);
  assert(lost.wasUp() && !lost.tooLong(600000) && lost.downMs(600000) == 0);
  lost.observe(false, 600000);
  assert(lost.limitMs() == 10000 && !lost.tooLong(609999) && lost.tooLong(610000));
  lost.observe(true, 610000);
  assert(!lost.tooLong(700000));
  lost.observe(false, UINT32_MAX - 1000);  // Rollover of millis(): 10s later is 8999.
  assert(!lost.tooLong(8998) && lost.tooLong(8999));
  puts("PASS: link loss record capture/complete and STA outage restart policy");
}
