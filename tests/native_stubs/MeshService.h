#pragma once
#include <stddef.h>
#include <stdint.h>
#include "RadioSettings.h"
class MeshEvents { public: virtual ~MeshEvents() = default; virtual void onNodeChanged() {} virtual void onRadioState(bool) {} };
struct MeshNode { char name[33]{}; struct { uint8_t pub_key[32]{}; } id; uint32_t lastSeen = 0; int16_t snrQuarter = 0; bool contact = false; };
class MeshService {
public:
    bool online = false, failStart = false;
    unsigned starts = 0, polls = 0, sends = 0;
    bool beginService() { ++starts; online = !failStart; return online; }
    bool setLoRaEnabled(bool enable) { if (enable && failStart) return false; online = enable; return true; }
    void loopService() { ++polls; }
    bool ready() const { return online; }
    bool sendPublic(const char *) { ++sends; return online; }
    bool sendDirect(size_t, const char *) { ++sends; return online; }
    void setEvents(MeshEvents *) {}
    size_t nodeCount() const { return 0; }
    const MeshNode *nodes() const { static MeshNode node; return &node; }
    bool setContact(size_t, bool) { return true; }
    const char *nodeName() const { return "test"; }
    bool setNodeName(const char *) { return true; }
    bool advertise() { return online; }
    static size_t radioPresetCount() { return 1; }
    size_t radioPresetIndex() const { return 0; }
    static RadioPreset radioPreset(size_t) { return {"test", 869.618f, 62.5f, 8, 8}; }
    bool selectRadioPreset(size_t index) { return index == 0; }
    int16_t lastRssi() const { return 0; }
    float lastSnr() const { return 0; }
};
extern MeshService meshService;
