#pragma once

// MeshCore's library build compiles its generic ESP32 helper even though this
// firmware supplies its own WioBoard in MeshService.cpp.  The helper expects a
// variant target header for declarations used only by its sleep path.
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/sensors/EnvironmentSensorManager.h>

extern CustomSX1262Wrapper radio_driver;
extern EnvironmentSensorManager sensors;

#ifndef P_LORA_NSS
#define P_LORA_NSS 8
#endif
