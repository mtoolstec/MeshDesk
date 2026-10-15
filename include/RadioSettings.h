#pragma once
#include <stdint.h>
struct RadioPreset {
    const char *name;
    float frequency;
    float bandwidth;
    uint8_t spreadingFactor;
    uint8_t codingRate;
};
