#include "MeshService.h"
#include "ChatStore.h"

#include <SPIFFS.h>
#include <RadioLib.h>
#include <esp_system.h>
#include <helpers/IdentityStore.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include "BoardPins.h"
#include "Hardware.h"
#include "SystemClock.h"
#include "WioRadio.h"

class WioBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override { return hardware.batteryMillivolts(); }
    const char *getManufacturerName() const override { return "Seeed Wio Tracker L2"; }
    void reboot() override { ESP.restart(); }
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
    uint32_t getIRQGpio() override { return pins::LORA_DIO1; }
};

static WioBoard board;
static SPIClass &loraSpi = wioRadioSpi();
static CustomSX1262 &radio = wioRadio();
static CustomSX1262Wrapper radioDriver(radio, board);
class EspHardwareRNG : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t size) override { esp_fill_random(dest, size); }
};
static EspHardwareRNG rng;
class ProtocolClock final : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override { return systemClock.now(); }
    void setCurrentTime(uint32_t timestamp) override { systemClock.set(timestamp); }
};
static ProtocolClock rtc;
static SimpleMeshTables tables;

static constexpr RadioPreset RADIO_PRESETS[] = {
    {"EU/UK Narrow", 869.618f, 62.5f, 8, 8},
    {"USA/Canada", 910.525f, 62.5f, 7, 5},
    {"Australia Narrow", 916.575f, 62.5f, 7, 8},
    {"New Zealand Narrow", 917.375f, 62.5f, 7, 5},
    {"Portugal 868", 869.618f, 62.5f, 7, 6},
    {"Switzerland", 869.618f, 62.5f, 8, 8},
    {"Vietnam Narrow", 920.250f, 62.5f, 8, 5},
};

MeshService meshService(radioDriver, rng, rtc, tables);

MeshService::MeshService(mesh::Radio &radioRef, mesh::RNG &rngRef, mesh::RTCClock &rtcRef,
                         mesh::MeshTables &tablesRef)
    : mesh::Mesh(radioRef, *new ArduinoMillis(), rngRef, rtcRef,
                 *new StaticPoolPacketManager(24), tablesRef), radio_(radioRef) {
    static const uint8_t publicSecret[16] = {
        0x8B, 0x33, 0x87, 0xE9, 0xC5, 0xCD, 0xEA, 0x6A,
        0xC9, 0xE5, 0xED, 0xBA, 0xA1, 0x15, 0xCD, 0x72
    };
    memcpy(publicChannel_.secret, publicSecret, sizeof(publicSecret));
    mesh::Utils::sha256(publicChannel_.hash, sizeof(publicChannel_.hash), publicSecret, sizeof(publicSecret));
}

bool initializeRadio() {
    Serial.println("Mesh: initializing SX1262");
    loraSpi.begin(pins::LORA_SCK, pins::LORA_MISO, pins::LORA_MOSI, pins::LORA_CS);
    if (!radio.std_init(&loraSpi)) {
        Serial.println("Mesh: SX1262 initialization failed");
        return false;
    }
    const RadioPreset &preset = MeshService::radioPreset(meshService.radioPresetIndex());
    radioDriver.setParams(preset.frequency, preset.bandwidth, preset.spreadingFactor, preset.codingRate);
    radioDriver.setTxPower(meshService.txPower());
    Serial.printf("Mesh: SX1262 ready (%s %.3f/BW%.1f/SF%u/CR%u)\n", preset.name,
                  preset.frequency, preset.bandwidth, preset.spreadingFactor, preset.codingRate);
    return true;
}

void MeshService::loadConfiguration() {
    // Read saved settings even when the protocol remains off; do not start RF.
    if (SPIFFS.begin(false)) loadRadioPreset();
}

bool MeshService::beginService() {
    if (!SPIFFS.begin(true)) {
        if (events_) events_->onRadioState(false);
        return false;
    }
    loadRadioPreset();
    if (!initializeRadio()) {
        if (events_) events_->onRadioState(false);
        return false;
    }
    IdentityStore identities(SPIFFS, "/identity");
    identities.begin();
    if (!identities.load("primary", self_id, nodeName_, sizeof(nodeName_))) {
        Serial.println("Mesh: creating identity");
        self_id = mesh::LocalIdentity(&rng);
        snprintf(nodeName_, sizeof(nodeName_), "Wio-%02X%02X", self_id.pub_key[0], self_id.pub_key[1]);
        identities.save("primary", self_id, nodeName_);
    }
    loadContacts();
    mesh::Mesh::begin();
    if (!loraEnabled_) {
        ready_ = false;
        radio.standby();
        radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
        radio.sleep(false);
        Serial.println("Mesh: LoRa disabled");
        if (events_) events_->onRadioState(false);
        return true;
    }
    Serial.println("Mesh: service online");
    ready_ = true;
    advertise();
    nextAdvert_ = millis() + 15UL * 60UL * 1000UL;
    if (events_) events_->onRadioState(true);
    return true;
}

size_t MeshService::radioPresetCount() { return sizeof(RADIO_PRESETS) / sizeof(RADIO_PRESETS[0]); }

const RadioPreset &MeshService::radioPreset(size_t index) {
    return RADIO_PRESETS[index < radioPresetCount() ? index : 0];
}

void MeshService::loadRadioPreset() {
    File file = SPIFFS.open("/radio_preset", "r");
    uint8_t index = 0;
    if (file && file.read(&index, 1) == 1 && index < radioPresetCount()) radioPresetIndex_ = index;
    if (file) file.close();
    file = SPIFFS.open("/tx_power", "r");
    int8_t power = LORA_TX_POWER;
    if (file && file.read(reinterpret_cast<uint8_t *>(&power), 1) == 1 && power >= 2 && power <= 22) txPower_ = power;
    if (file) file.close();
    file = SPIFFS.open("/broadcast_gps", "r");
    uint8_t broadcast = 0;
    if (file && file.read(&broadcast, 1) == 1) broadcastLocation_ = broadcast != 0;
    if (file) file.close();
    file = SPIFFS.open("/lora_enabled", "r");
    uint8_t lora = 1;
    if (file && file.read(&lora, 1) == 1) loraEnabled_ = lora != 0;
    if (file) file.close();
}

bool MeshService::saveRadioPreset() {
    if (!SPIFFS.begin(false)) return false;
    File file = SPIFFS.open("/radio_preset", "w");
    if (!file) return false;
    const uint8_t index = radioPresetIndex_;
    const bool saved = file.write(&index, 1) == 1; file.close();
    return saved;
}

void MeshService::saveLoRaEnabled() {
    File file = SPIFFS.open("/lora_enabled", "w");
    if (file) { const uint8_t value = loraEnabled_ ? 1 : 0; file.write(&value, 1); file.close(); }
}

void MeshService::loadContacts() {
    File file = SPIFFS.open("/contacts", "r");
    uint8_t count = 0;
    if (file) file.read(&count, 1);
    for (uint8_t i = 0; i < count && nodeCount_ < MAX_NODES; ++i) {
        MeshNode &node = nodes_[nodeCount_];
        if (file.read(node.id.pub_key, PUB_KEY_SIZE) != PUB_KEY_SIZE ||
            file.read(reinterpret_cast<uint8_t *>(node.name), sizeof(node.name)) != sizeof(node.name)) break;
        node.name[sizeof(node.name) - 1] = 0;
        node.contact = true;
        ++nodeCount_;
    }
    file.close();
    for (size_t i = 0; i < chatStore.peerCount(); ++i) {
        const auto *saved = chatStore.peer(i); if (strcmp(saved->protocol, "meshcore") || strlen(saved->peer.id) != 64) continue;
        mesh::Identity identity{}; bool valid = true;
        for (unsigned j = 0; j < 32; ++j) {
            char pair[3] = {saved->peer.id[j * 2], saved->peer.id[j * 2 + 1], 0}; unsigned byte = 0;
            if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%x", &byte) != 1) { valid = false; break; }
            identity.pub_key[j] = byte;
        }
        if (!valid) continue;
        size_t index = 0; while (index < nodeCount_ && memcmp(nodes_[index].id.pub_key, identity.pub_key, 32)) ++index;
        if (index == MAX_NODES) continue;
        if (index == nodeCount_) { nodes_[index] = {}; nodes_[index].id = identity; ++nodeCount_; }
        snprintf(nodes_[index].name, sizeof(nodes_[index].name), "%s", saved->peer.name); nodes_[index].contact = saved->peer.contact;
    }
    Serial.printf("Mesh: loaded %u contacts\n", unsigned(nodeCount_));
}

void MeshService::saveContacts() {
    File file = SPIFFS.open("/contacts", "w");
    if (!file) return;
    uint8_t count = 0;
    for (size_t i = 0; i < nodeCount_; ++i) if (nodes_[i].contact) ++count;
    file.write(&count, 1);
    for (size_t i = 0; i < nodeCount_; ++i) if (nodes_[i].contact) {
        file.write(nodes_[i].id.pub_key, PUB_KEY_SIZE);
        file.write(reinterpret_cast<const uint8_t *>(nodes_[i].name), sizeof(nodes_[i].name));
    }
    file.close();
}

bool MeshService::setContact(size_t nodeIndex, bool contact) {
    if (nodeIndex >= nodeCount_) return false;
    nodes_[nodeIndex].contact = contact;
    saveContacts();
    if (events_) events_->onNodeChanged();
    return true;
}

bool MeshService::setNodeName(const char *name) {
    if (!name || !name[0]) return false;
    strlcpy(nodeName_, name, sizeof(nodeName_));
    IdentityStore identities(SPIFFS, "/identity");
    identities.begin();
    identities.save("primary", self_id, nodeName_);
    advertise();
    return true;
}

void MeshService::setTxPower(int8_t power) {
    txPower_ = power;
    if (ready_) radioDriver.setTxPower(power);
    File file = SPIFFS.open("/tx_power", "w");
    if (file) { file.write(reinterpret_cast<uint8_t *>(&txPower_), 1); file.close(); }
}

void MeshService::setBroadcastLocation(bool enabled) {
    broadcastLocation_ = enabled;
    File file = SPIFFS.open("/broadcast_gps", "w");
    if (file) { const uint8_t value = enabled ? 1 : 0; file.write(&value, 1); file.close(); }
    advertise();
}

bool MeshService::setLoRaEnabled(bool enabled) {
    if (enabled == loraEnabled_) return true;
    if (!enabled) {
        loraEnabled_ = false;
        saveLoRaEnabled();
        ready_ = false;
        radio.standby();
        radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
        radio.sleep(false);
        Serial.println("Mesh: LoRa disabled");
        if (events_) events_->onRadioState(false);
        return true;
    }
    if (!initializeRadio()) {
        Serial.println("Mesh: LoRa enable failed (radio unavailable)");
        if (events_) events_->onRadioState(false);
        return false;
    }
    mesh::Mesh::begin();
    loraEnabled_ = true;
    ready_ = true;
    saveLoRaEnabled();
    Serial.println("Mesh: LoRa enabled");
    if (events_) events_->onRadioState(true);
    advertise();
    nextAdvert_ = millis() + 15UL * 60UL * 1000UL;
    return true;
}

bool MeshService::selectRadioPreset(size_t index) {
    if (index >= radioPresetCount()) return false;
    const size_t previous = radioPresetIndex_;
    radioPresetIndex_ = index;
    if (!saveRadioPreset()) { radioPresetIndex_ = previous; return false; }
    if (ready_) {
        const RadioPreset &preset = radioPreset(index);
        radioDriver.resetAGC();
        radioDriver.setParams(preset.frequency, preset.bandwidth, preset.spreadingFactor, preset.codingRate);
        Serial.printf("Mesh: preset changed to %s\n", preset.name);
    }
    return true;
}

void MeshService::loopService() {
    systemClock.now();
    if (!ready_) return;
    mesh::Mesh::loop();
    if (int32_t(millis() - nextAdvert_) >= 0) {
        advertise();
        nextAdvert_ = millis() + 15UL * 60UL * 1000UL;
    }
}

bool MeshService::advertise() {
    if (!ready_) return false;
    uint8_t data[MAX_ADVERT_DATA_SIZE];
    uint8_t len;
    if (broadcastLocation_ && hardware.gpsHasFix()) {
        AdvertDataBuilder builder(ADV_TYPE_CHAT, nodeName_, hardware.gpsLatitude(), hardware.gpsLongitude());
        len = builder.encodeTo(data);
    } else {
        AdvertDataBuilder builder(ADV_TYPE_CHAT, nodeName_);
        len = builder.encodeTo(data);
    }
    auto *packet = createAdvert(self_id, data, len);
    if (!packet) {
        Serial.println("Mesh: advert failed (packet pool empty)");
        return false;
    }
    // MeshCore Companion uses zero-hop for a user-triggered self advert.
    // It is sent immediately to nearby nodes instead of waiting in the
    // de-prioritized flood queue.
    sendZeroHop(packet);
    Serial.printf("Mesh: advert queued (%s)\n", nodeName_);
    return true;
}

bool MeshService::floodAdvertise() {
    if (!ready_) return false;
    uint8_t data[MAX_ADVERT_DATA_SIZE];
    uint8_t len;
    if (broadcastLocation_ && hardware.gpsHasFix()) {
        AdvertDataBuilder builder(ADV_TYPE_CHAT, nodeName_, hardware.gpsLatitude(), hardware.gpsLongitude());
        len = builder.encodeTo(data);
    } else {
        AdvertDataBuilder builder(ADV_TYPE_CHAT, nodeName_);
        len = builder.encodeTo(data);
    }
    auto *packet = createAdvert(self_id, data, len);
    if (!packet) return false;
    sendFlood(packet);
    Serial.printf("Mesh: flood advert queued (%s)\n", nodeName_);
    return true;
}

void MeshService::logTx(mesh::Packet *packet, int len) {
    if (packet && packet->getPayloadType() == PAYLOAD_TYPE_ADVERT) {
        Serial.printf("Mesh: advert transmitted (%d bytes)\n", len);
        if (events_) events_->onAdvertTransmitted(packet->isRouteFlood());
    }
}

bool MeshService::sendPublic(const char *text) {
    if (!ready_ || !text || !text[0]) return false;
    uint8_t data[MAX_PACKET_PAYLOAD];
    const uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
    memcpy(data, &timestamp, sizeof(timestamp));
    data[4] = TXT_TYPE_PLAIN;
    const int prefix = snprintf(reinterpret_cast<char *>(&data[5]), sizeof(data) - 5, "%s: ", nodeName_);
    const size_t room = sizeof(data) - 6 - prefix;
    const size_t length = strnlen(text, room);
    memcpy(&data[5 + prefix], text, length);
    data[5 + prefix + length] = 0;
    auto *packet = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, publicChannel_, data, 5 + prefix + length);
    if (!packet) return false;
    sendFlood(packet);
    if (events_) events_->onMeshMessage(text, timestamp, false, true, false, nodeName_);
    return true;
}

bool MeshService::sendDirect(size_t nodeIndex, const char *text) {
    if (!ready_ || nodeIndex >= nodeCount_ || !text || !text[0]) return false;
    uint8_t data[MAX_PACKET_PAYLOAD];
    const uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
    memcpy(data, &timestamp, sizeof(timestamp));
    data[4] = TXT_TYPE_PLAIN << 2;
    const size_t length = strnlen(text, sizeof(data) - 5);
    memcpy(&data[5], text, length);
    uint8_t secret[PUB_KEY_SIZE];
    self_id.calcSharedSecret(secret, nodes_[nodeIndex].id);
    auto *packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, nodes_[nodeIndex].id, secret, data, 5 + length);
    if (!packet) return false;
    sendFlood(packet);
    char peerId[65]{}; for (unsigned i = 0; i < 32; ++i) snprintf(peerId + 2 * i, 3, "%02x", nodes_[nodeIndex].id.pub_key[i]);
    if (events_) events_->onMeshMessage(text, timestamp, true, true, nodes_[nodeIndex].contact, nodes_[nodeIndex].name, peerId);
    return true;
}

int MeshService::searchChannelsByHash(const uint8_t *hash, mesh::GroupChannel channels[], int maxMatches) {
    if (maxMatches > 0 && hash[0] == publicChannel_.hash[0]) {
        channels[0] = publicChannel_;
        return 1;
    }
    return 0;
}

void MeshService::onGroupDataRecv(mesh::Packet *, uint8_t type, const mesh::GroupChannel &,
                                  uint8_t *data, size_t len) {
    if (type != PAYLOAD_TYPE_GRP_TXT || len < 6) return;
    uint32_t timestamp;
    memcpy(&timestamp, data, sizeof(timestamp));
    char message[MAX_PACKET_PAYLOAD];
    const size_t textLength = min(len - 5, sizeof(message) - 1);
    memcpy(message, &data[5], textLength);
    message[textLength] = 0;
    char sender[33]{}; const char *body = message;
    const char *separator = strstr(message, ": ");
    if (separator) {
        const size_t senderLength = min(size_t(separator - message), sizeof(sender) - 1);
        memcpy(sender, message, senderLength); sender[senderLength] = 0; body = separator + 2;
    }
    bool knownContact = false;
    for (size_t i = 0; sender[0] && i < nodeCount_; ++i) {
        if (nodes_[i].contact && !strcmp(nodes_[i].name, sender)) { knownContact = true; break; }
    }
    if (events_) events_->onMeshMessage(body, timestamp, false, false, knownContact, sender);
}

void MeshService::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                               const uint8_t *appData, size_t appDataLen) {
    (void)timestamp;
    AdvertDataParser advert(appData, appDataLen);
    if (!advert.isValid()) return;
    const char *name = advert.hasName() ? advert.getName() : "Unnamed node";
    size_t slot = nodeCount_;
    for (size_t i = 0; i < nodeCount_; ++i) {
        if (memcmp(nodes_[i].id.pub_key, id.pub_key, PUB_KEY_SIZE) == 0) { slot = i; break; }
    }
    if (slot == nodeCount_ && nodeCount_ < MAX_NODES) ++nodeCount_;
    if (slot >= MAX_NODES) slot = MAX_NODES - 1;
    strlcpy(nodes_[slot].name, name, sizeof(nodes_[slot].name));
    // Use the local receive time for "last heard". The signed advert timestamp
    // belongs to the sender and can be skewed from this device's clock.
    nodes_[slot].lastSeen = getRTCClock()->getCurrentTime();
    nodes_[slot].snrQuarter = int8_t(packet->getSNR() * 4.0f);
    nodes_[slot].id = id;
    Serial.printf("Mesh: advert %s, SNR %.1f dB\n", nodes_[slot].name, packet->getSNR());
    if (events_) events_->onAdvertNotification(nodes_[slot].name, nodes_[slot].snrQuarter);
    if (events_) events_->onNodeChanged();
}

int MeshService::searchPeersByHash(const uint8_t *hash) {
    int matches = 0;
    for (size_t i = 0; i < nodeCount_ && matches < int(MAX_NODES); ++i) {
        if (nodes_[i].id.isHashMatch(hash)) matchingNodes_[matches++] = i;
    }
    return matches;
}

void MeshService::getPeerSharedSecret(uint8_t *destSecret, int peerIndex) {
    if (peerIndex >= 0 && peerIndex < int(MAX_NODES)) {
        const int nodeIndex = matchingNodes_[peerIndex];
        if (nodeIndex >= 0 && nodeIndex < int(nodeCount_)) self_id.calcSharedSecret(destSecret, nodes_[nodeIndex].id);
    }
}

void MeshService::onPeerDataRecv(mesh::Packet *, uint8_t type, int senderIndex,
                                 const uint8_t *, uint8_t *data, size_t len) {
    if (type != PAYLOAD_TYPE_TXT_MSG || len < 6 || senderIndex < 0 || senderIndex >= int(MAX_NODES)) return;
    const int nodeIndex = matchingNodes_[senderIndex];
    if (nodeIndex < 0 || nodeIndex >= int(nodeCount_)) return;
    uint32_t timestamp;
    memcpy(&timestamp, data, sizeof(timestamp));
    char text[96];
    const size_t textLength = min(len - 5, sizeof(text) - 1);
    memcpy(text, &data[5], textLength);
    text[textLength] = 0;
    char peerId[65]{}; for (unsigned i = 0; i < 32; ++i) snprintf(peerId + 2 * i, 3, "%02x", nodes_[nodeIndex].id.pub_key[i]);
    if (events_) events_->onMeshMessage(text, timestamp, true, false,
                                        nodes_[nodeIndex].contact, nodes_[nodeIndex].name, peerId);
}

int16_t MeshService::lastRssi() const { return ready_ ? int16_t(radio_.getLastRSSI()) : 0; }
float MeshService::lastSnr() const { return ready_ ? radio_.getLastSNR() : 0; }
