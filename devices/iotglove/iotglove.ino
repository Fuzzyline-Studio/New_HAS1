#include "iotglove.h"

// SecureOTA CI increments the sketch version before compile/sign/release.
#define FIRMWARE_VER 26
#define PARTITION_VER 1

void setup() { gloveBegin(FIRMWARE_VER, PARTITION_VER); }
void loop() { gloveLoop(); }
