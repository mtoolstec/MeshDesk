#pragma once
#include <stdint.h>

// Protocol-independent epoch clock. Calling now() also handles millis() wrap.
class SystemClock {
public:
    uint32_t now();
    void set(uint32_t timestamp);
private:
    uint32_t base_ = 1715770351;
    uint32_t previous_ = 0;
    uint64_t elapsed_ = 0;
};
extern SystemClock systemClock;
