#pragma once

#include <Arduino.h>
#include "RadioSettings.h"
#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/TxtDataHelpers.h>

struct MeshNode {
    char name[33];
    uint32_t lastSeen;
    int8_t snrQuarter;
    mesh::Identity id;
    bool contact;
};


class MeshService;

class MeshEvents {
public:
    virtual ~MeshEvents() = default;
    virtual void onMeshMessage(const char *text, uint32_t timestamp, bool direct, bool outgoing,
                               bool contact, const char *peer, const char *peerId = "", uint32_t packetId = 0) = 0;
    virtual void onMeshData(const char *, const char *, uint32_t, uint16_t, const uint8_t *, size_t, bool, uint32_t) {}
    virtual void onNodeChanged() = 0;
    virtual void onRadioState(bool ready) = 0;
    virtual void onAdvertNotification(const char *name, int16_t snrQuarter) = 0;
    virtual void onAdvertTransmitted(bool flood) = 0;
};

class MeshService : public mesh::Mesh {
public:
    MeshService(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, mesh::MeshTables &tables);
    bool beginService();
    void loadConfiguration();
    void loopService();
    bool sendPublic(const char *text);
    bool sendDirect(size_t nodeIndex, const char *text);
    bool advertise();
    bool floodAdvertise();
    bool selectRadioPreset(size_t index);
    bool setNodeName(const char *name);
    bool setContact(size_t nodeIndex, bool contact);
    void setTxPower(int8_t power);
    void setBroadcastLocation(bool enabled);
    bool setLoRaEnabled(bool enabled);
    bool loraEnabled() const { return loraEnabled_; }
    bool broadcastLocation() const { return broadcastLocation_; }
    int8_t txPower() const { return txPower_; }
    size_t radioPresetIndex() const { return radioPresetIndex_; }
    static size_t radioPresetCount();
    static const RadioPreset &radioPreset(size_t index);
    void setEvents(MeshEvents *events) { events_ = events; }
    const MeshNode *nodes() const { return nodes_; }
    size_t nodeCount() const { return nodeCount_; }
    const char *nodeName() const { return nodeName_; }
    bool ready() const { return ready_; }
    int16_t lastRssi() const;
    float lastSnr() const;
    uint32_t sentFloodCount() const { return getNumSentFlood(); }
    uint32_t sentDirectCount() const { return getNumSentDirect(); }
    uint32_t currentTime() { return getRTCClock()->getCurrentTime(); }
    void setCurrentTime(uint32_t timestamp) { getRTCClock()->setCurrentTime(timestamp); }

protected:
    bool allowPacketForward(const mesh::Packet *packet) override { return true; }
    void onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                      const uint8_t *appData, size_t appDataLen) override;
    int searchChannelsByHash(const uint8_t *hash, mesh::GroupChannel channels[], int maxMatches) override;
    void onGroupDataRecv(mesh::Packet *packet, uint8_t type, const mesh::GroupChannel &channel,
                         uint8_t *data, size_t len) override;
    int searchPeersByHash(const uint8_t *hash) override;
    void getPeerSharedSecret(uint8_t *destSecret, int peerIndex) override;
    void onPeerDataRecv(mesh::Packet *packet, uint8_t type, int senderIndex,
                        const uint8_t *secret, uint8_t *data, size_t len) override;
    void logTx(mesh::Packet *packet, int len) override;

private:
    static constexpr size_t MAX_NODES = 32;
    mesh::Radio &radio_;
    MeshEvents *events_ = nullptr;
    mesh::GroupChannel publicChannel_{};
    MeshNode nodes_[MAX_NODES]{};
    size_t nodeCount_ = 0;
    char nodeName_[33] = "Wio L2";
    bool ready_ = false;
    uint32_t nextAdvert_ = 0;
    size_t radioPresetIndex_ = 0;
    int8_t txPower_ = LORA_TX_POWER;
    bool broadcastLocation_ = false;
    bool loraEnabled_ = true;
    int matchingNodes_[MAX_NODES]{};
    void loadRadioPreset();
    bool saveRadioPreset();
    void saveLoRaEnabled();
    void loadContacts();
    void saveContacts();
};

extern MeshService meshService;
bool initializeRadio();
