#pragma once
#include "CommunicationService.h"
class MeshtasticService final : public CommunicationBackend {
public:
    void loadConfiguration();
    const char *name() const override { return "meshtastic"; }
    bool start() override;
    bool stop() override;
    void poll() override;
    bool ready() const override;
    bool sendPublic(const char *) override;
    bool sendDirect(size_t, const char *) override;
    void setEvents(MeshEvents *) override;
    size_t peerCount() const override;
    CommunicationPeer peer(size_t) const override;
    bool setContact(size_t, bool) override;
    const char *nodeName() const override;
    bool setNodeName(const char *) override;
    bool advertise() override;
    size_t presetCount() const override;
    size_t presetIndex() const override;
    RadioPreset preset(size_t) const override;
    bool selectPreset(size_t) override;
    int16_t rssi() const override;
    float snr() const override;
    const char *error() const override;
    size_t regionCount() const;
    const char *regionName(size_t) const;
    size_t regionIndex() const;
    bool selectRegion(size_t);
    size_t roleIndex() const;
    const char *roleName(size_t) const;
    bool selectRole(size_t);
    bool signalKnown() const override;
    bool setFrequency(float mhz);
    bool setChannel(const char *name, const char *base64Key);
    const char *channelName() const;
    uint32_t nodeNumber() const;
    bool publicKey(uint8_t output[32]) const;
    bool peerPublicKey(size_t index, uint8_t output[32]) const;
    bool importPublicKey(uint32_t number, const uint8_t key[32]);
    bool forgetPeer(uint32_t number);
    uint32_t txCount() const;
    uint32_t rxCount() const;
    uint32_t rejectedCount() const;
    uint32_t lastTxId() const;
    uint32_t lastAckId() const;
    unsigned queuedCount() const;
    // Exercises the production decode path without a second RF device.
    bool receiveFrame(const uint8_t *frame, size_t length, float rssi, float snr);
};
extern MeshtasticService meshtasticService;
