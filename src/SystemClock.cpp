#include "SystemClock.h"
#include <Arduino.h>
SystemClock systemClock;
uint32_t SystemClock::now() {
    const uint32_t current = millis();
    elapsed_ += uint32_t(current - previous_);
    previous_ = current;
    return base_ + uint32_t(elapsed_ / 1000);
}
void SystemClock::set(uint32_t timestamp) {
    base_ = timestamp;
    previous_ = millis();
    elapsed_ = 0;
}
