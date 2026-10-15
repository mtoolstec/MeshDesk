#pragma once
#include "CommunicationService.h"
class MeshtasticService final : public CommunicationBackend {
public:
    bool online = false, failStart = false, failStop = false;
    unsigned starts = 0, stops = 0, polls = 0, sends = 0;
    const char *name() const override { return "meshtastic"; }
    bool start() override { ++starts; online = !failStart; return online; }
    bool stop() override { ++stops; if (failStop) return false; online = false; return true; }
    void poll() override { ++polls; }
    bool ready() const override { return online; }
    bool sendPublic(const char *) override { ++sends; return online; }
    bool sendDirect(size_t, const char *) override { ++sends; return online; }
};
extern MeshtasticService meshtasticService;
