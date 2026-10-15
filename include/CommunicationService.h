#pragma once
#include <stddef.h>
#include <stdint.h>
#include "RadioSettings.h"
class MeshEvents;
struct CommunicationPeer {
    char name[40]{};
    char id[70]{};
    uint32_t lastSeen = 0;
    int16_t snrQuarter = 0;
    bool contact = false;
    bool hasPublicKey = false;
};
class CommunicationBackend {
public:
    virtual ~CommunicationBackend() = default;
    virtual const char *name() const = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;
    virtual void poll() = 0;
    virtual bool ready() const = 0;
    virtual bool sendPublic(const char *message) = 0;
    virtual bool sendDirect(size_t peer, const char *message) = 0;
    virtual void setEvents(MeshEvents *) {}
    virtual size_t peerCount() const { return 0; }
    virtual CommunicationPeer peer(size_t) const { return {}; }
    virtual bool setContact(size_t, bool) { return false; }
    virtual const char *nodeName() const { return ""; }
    virtual bool setNodeName(const char *) { return false; }
    virtual bool advertise() { return false; }
    virtual size_t presetCount() const { return 0; }
    virtual size_t presetIndex() const { return 0; }
    virtual RadioPreset preset(size_t) const { return {}; }
    virtual bool selectPreset(size_t) { return false; }
    virtual int16_t rssi() const { return 0; }
    virtual float snr() const { return 0; }
    virtual bool signalKnown() const { return ready() && rssi() < 0; }
    virtual const char *error() const { return ""; }
};
class CommunicationService {
public:
    bool begin();
    void poll();
    bool select(const char *protocol);
    const char *protocol() const;
    const char *configurationProtocol() const;
    const char *error() const;
    bool ready() const;
    bool sendPublic(const char *message);
    bool sendDirect(size_t peer, const char *message);
    void setEvents(MeshEvents *events);
    size_t peerCount() const;
    CommunicationPeer peer(size_t index) const;
    bool setContact(size_t index, bool enabled);
    const char *nodeName() const;
    bool setNodeName(const char *name);
    bool advertise();
    size_t radioPresetCount() const;
    size_t radioPresetIndex() const;
    RadioPreset radioPreset(size_t index) const;
    bool selectRadioPreset(size_t index);
    int16_t rssi() const;
    float snr() const;
    bool signalKnown() const;
private:
    CommunicationBackend *active_ = nullptr;
    CommunicationBackend *configured_ = nullptr;
    MeshEvents *events_ = nullptr;
    const char *error_ = "";
};
extern CommunicationService communication;
