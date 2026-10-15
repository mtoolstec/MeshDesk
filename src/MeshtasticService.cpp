#include "MeshtasticService.h"
#include "MeshService.h"
#include "WioRadio.h"
#include "BoardPins.h"
#include "SystemClock.h"
#include <Preferences.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <helpers/radiolib/CustomSX1262.h>
#define MESH_CRYPTO_USE_MBEDTLS 1
#define MESH_MAX_NODE_KEYS 32
#include <meshtastic.h>
#include "MeshtasticResponses.h"
#include "MeshtasticRelay.h"
#include "ChatStore.h"

namespace {
struct Config {
    uint32_t magic = 0x4d544301;
    uint8_t region = REGION_US, preset = MODEM_LONG_FAST;
    uint8_t pskLength = 1;
    uint8_t psk[32] = {1};
    float frequency = 0;
    char channel[32]{};
    char name[25] = "Wio Tracker L2";
};
struct Node { CommunicationPeer peer; uint32_t number = 0; };
struct Tx { uint8_t frame[255]{}; uint16_t length = 0; bool advert = false, relay = false; };
struct Relay { Tx tx{}; uint32_t due = 0; };
struct RelaySeen { uint32_t from = 0, id = 0, at = 0; };
struct KeyRequest { uint32_t number = 0, at = 0; };
struct PendingDm { uint8_t frame[255]{}; uint16_t length = 0; uint32_t expires = 0; float rssi = 0, snr = 0; };
struct SavedNode { uint32_t number; uint8_t key[32]; char name[40]; uint8_t contact, hasKey; };
struct SavedNodes { uint32_t magic = 0x4d544e01; uint32_t count = 0; SavedNode nodes[32]{}; };
struct Seen { uint32_t from = 0, id = 0; };
struct State {
    Config config{};
    uint8_t role = ROLE_CLIENT_MUTE;
    bool signalKnown = false;
    Relay relays[2]{}; RelaySeen relaySeen[64]{}; unsigned relaySeenAt = 0;
    MeshSession session{};
    MeshEvents *events = nullptr;
    bool loaded = false, initialized = false, online = false, transmitting = false;
    Node nodes[32]{}; size_t nodeCount = 0;
    PendingDm pending[2]{};
    KeyRequest keyRequests[4]{};
    Seen seen[64]{}; unsigned seenAt = 0;
    Tx queue[4]{}; unsigned head = 0, count = 0;
    uint32_t due = 0, txDeadline = 0, nextAdvert = 0, nextPoll = 0;
    uint32_t tx = 0, rx = 0, rejected = 0, txId = 0, ackId = 0;
    int16_t rssi = 0; float snr = 0;
    char error[96]{};
} state;
uint32_t hardwareRandom() { return esp_random(); }
void failure(const char *operation, int code) { snprintf(state.error, sizeof(state.error), "%s (%d)", operation, code); }
bool validConfig(const Config &c) {
    if (c.magic != 0x4d544301 || c.region >= REGION_UNSET || c.preset >= MODEM_PRESET_COUNT ||
        (c.pskLength != 0 && c.pskLength != 1 && c.pskLength != 16 && c.pskLength != 32) ||
        !memchr(c.channel, 0, sizeof(c.channel)) || !memchr(c.name, 0, sizeof(c.name)) || !c.name[0] || !isfinite(c.frequency)) return false;
    const auto *region = meshGetRegion(MeshRegion(c.region));
    const auto config = meshBuildRadioConfig(MeshRegion(c.region), MeshModemPreset(c.preset), c.channel[0] ? c.channel : meshPresetName(MeshModemPreset(c.preset)));
    const float freq = c.frequency == 0 ? config.frequency_mhz : c.frequency;
    const float half = config.bandwidth_khz / 2000;
    return freq - half >= region->freq_start - .001f && freq + half <= region->freq_end + .001f;
}
bool saveConfig() {
    Preferences p; if (!p.begin("meshtastic", false)) return false;
    const bool ok = p.putBytes("config", &state.config, sizeof(state.config)) == sizeof(state.config); p.end(); return ok;
}
bool saveNodes() {
    SavedNodes saved{}; saved.count = state.nodeCount;
    for (size_t i = 0; i < saved.count; ++i) {
        auto &entry = saved.nodes[i]; const auto &node = state.nodes[i];
        entry.number = node.number; entry.contact = node.peer.contact;
        snprintf(entry.name, sizeof(entry.name), "%s", node.peer.name);
        if (const auto *key = state.session.node_keys.getKey(node.number)) {
            memcpy(entry.key, key, sizeof(entry.key)); entry.hasKey = 1;
        }
    }
    Preferences p; if (!p.begin("meshtastic", false)) return false;
    const bool ok = p.putBytes("nodes", &saved, sizeof(saved)) == sizeof(saved); p.end(); return ok;
}
void restoreNodes() {
    SavedNodes saved{}; Preferences p; p.begin("meshtastic", true);
    const size_t length = p.getBytes("nodes", &saved, sizeof(saved)); p.end();
    if (length != sizeof(saved) || saved.magic != 0x4d544e01 || saved.count > 32) return;
    for (size_t i = 0; i < saved.count; ++i) {
        const auto &entry = saved.nodes[i];
        if (entry.number < 4 || entry.number == MESH_ADDR_BROADCAST || entry.number == state.session.node_num ||
            !memchr(entry.name, 0, sizeof(entry.name)) || entry.contact > 1 || entry.hasKey > 1) return;
        for (size_t j = 0; j < i; ++j) if (saved.nodes[j].number == entry.number) return;
    }
    state.nodeCount = saved.count;
    for (size_t i = 0; i < saved.count; ++i) {
        auto &node = state.nodes[i]; const auto &entry = saved.nodes[i]; node.number = entry.number;
        snprintf(node.peer.id, sizeof(node.peer.id), "!%08lx", (unsigned long)node.number);
        snprintf(node.peer.name, sizeof(node.peer.name), "%s", entry.name);
        node.peer.contact = entry.contact; node.peer.hasPublicKey = entry.hasKey;
        if (entry.hasKey) state.session.node_keys.setKey(node.number, entry.key);
    }
}
void setupSessionConfig() {
    auto &session = state.session;
    session.role = MeshRole(state.role);
    session.region = MeshRegion(state.config.region); session.preset = MeshModemPreset(state.config.preset);
    session.channels.init(session.preset);
    session.channels.addChannel(state.config.channel, state.config.psk, state.config.pskLength, true);
}
int configureRadio() {
    auto config = state.session.radioConfig(19);
    if (state.config.frequency) config.frequency_mhz = state.config.frequency;
    auto &radio = wioRadio();
    wioRadioSpi().begin(pins::LORA_SCK, pins::LORA_MISO, pins::LORA_MOSI, pins::LORA_CS);
    radio.clearDio1Action();
    int code = radio.begin(config.frequency_mhz, config.bandwidth_khz, config.spreading_factor,
        config.coding_rate, config.sync_word, config.tx_power_dbm, config.preamble_length, 1.8f, false);
    if (code == RADIOLIB_ERR_SPI_CMD_FAILED || code == RADIOLIB_ERR_SPI_CMD_INVALID)
        code = radio.begin(config.frequency_mhz, config.bandwidth_khz, config.spreading_factor,
            config.coding_rate, config.sync_word, config.tx_power_dbm, config.preamble_length, 0.0f, false);
    if (code) return code;
    if ((code = radio.setDio2AsRfSwitch(true))) return code;
    if ((code = radio.setCRC(2))) return code;
    if ((code = radio.explicitHeader())) return code;
    if ((code = radio.setRxBoostedGainMode(true))) return code;
    return radio.startReceive();
}
bool applyConfig(const Config &config) {
    if (!validConfig(config)) { failure("Invalid Meshtastic configuration", -1); return false; }
    if (state.count || state.transmitting) { failure("Wait for TX queue to finish", -1); return false; }
    const auto previous = state.config; state.config = config; setupSessionConfig();
    int code = state.online ? configureRadio() : 0;
    if (code || !saveConfig()) {
        state.config = previous; setupSessionConfig();
        if (state.online && configureRadio()) state.online = false;
        failure(code ? "Radio configuration failed" : "Could not save configuration", code); return false;
    }
    memset(state.seen, 0, sizeof(state.seen)); state.error[0] = 0;
    memset(state.relaySeen, 0, sizeof(state.relaySeen));
    for (auto &relay : state.relays) relay.tx.length = 0;
    state.signalKnown = false; state.rssi = 0; state.snr = 0;
    if (state.online) state.nextAdvert = millis() + 5000;
    return true;
}
bool enqueue(const uint8_t *frame, size_t length, bool advert = false, bool relay = false) {
    if (!state.online || length <= 16 || length > 255 || state.count == 4) { failure("TX queue unavailable", -1); return false; }
    auto &tx = state.queue[(state.head + state.count) % 4];
    memcpy(tx.frame, frame, length); tx.length = length; tx.advert = advert; tx.relay = relay;
    if (!state.count) state.due = millis() + (hardwareRandom() % 8) * meshSlotTimeMs(state.session.preset);
    ++state.count; state.error[0] = 0; return true;
}
bool samePacket(const Tx &tx, const MeshRxPacket &packet) {
    uint32_t from = 0, id = 0;
    if (tx.length <= 16) return false;
    memcpy(&from, tx.frame + 4, 4); memcpy(&id, tx.frame + 8, 4);
    return from == packet.from && id == packet.id;
}
bool relayDuplicate(const MeshRxPacket &packet) {
    for (const auto &seen : state.relaySeen) if (seen.from == packet.from && seen.id == packet.id &&
            uint32_t(millis() - seen.at) < 10UL * 60 * 1000) {
        // A different station's rebroadcast makes our pending client relay unnecessary.
        if (packet.relay_node != uint8_t(state.session.node_num)) {
            for (auto &relay : state.relays) if (samePacket(relay.tx, packet)) relay.tx.length = 0;
            unsigned kept = 0;
            for (unsigned i = 0; i < state.count; ++i) {
                const auto tx = state.queue[(state.head + i) % 4];
                if (!(i == 0 && state.transmitting) && tx.relay && samePacket(tx, packet)) continue;
                state.queue[(state.head + kept++) % 4] = tx;
            }
            state.count = kept;
        }
        return true;
    }
    return false;
}
void scheduleRelay(const uint8_t *frame, size_t length, const MeshRxPacket &packet, float snr) {
    if (state.role != ROLE_CLIENT || relayDuplicate(packet)) return;
    for (auto &relay : state.relays) if (!relay.tx.length) {
        const size_t size = buildClientRelay(frame, length, state.session.node_num, true,
                                            relay.tx.frame, sizeof(relay.tx.frame));
        if (!size) return;
        relay.tx.length = size; relay.tx.relay = true;
        relay.due = millis() + meshRebroadcastDelayMs(state.session.preset, ROLE_CLIENT, snr);
        state.relaySeen[state.relaySeenAt++ % 64] = {packet.from, packet.id, millis()};
        return;
    }
}
bool queueNodeInfo(uint32_t to, bool wantResponse) {
    char id[12], shortName[5]; snprintf(id, sizeof(id), "!%08lx", (unsigned long)state.session.node_num);
    snprintf(shortName, sizeof(shortName), "%04x", unsigned(state.session.node_num & 0xffff));
    uint8_t user[160], data[200], frame[255];
    size_t length = meshEncodeUser(user, sizeof(user), id, state.config.name, shortName, 137, state.session.pki.public_key, 32);
    auto userWriter = pbWriter(user + length, sizeof(user) - length);
    userWriter.writeVarintField(7, state.role); length += userWriter.written();
    size_t dataLen = meshEncodeData(data, sizeof(data), PORT_NODEINFO, user, length, wantResponse, false);
    auto writer = pbWriter(data + dataLen, sizeof(data) - dataLen);
    writer.writeFixed32Field(4, to); writer.writeFixed32Field(5, state.session.node_num); dataLen += writer.written();
    const uint32_t pktId = state.session.id_gen.next(); const auto &channel = state.session.channels.channels[0];
    if (!dataLen || !meshEncrypt(&channel.key, state.session.node_num, pktId, data, dataLen)) return false;
    length = meshBuildPacket(frame, to, state.session.node_num, pktId, 3, 3, false, false,
        channel.hash, 0, uint8_t(state.session.node_num), data, dataLen);
    return enqueue(frame, length, true);
}
void requestKey(uint32_t number) {
    const uint32_t now = millis();
    KeyRequest *slot = &state.keyRequests[0];
    for (auto &entry : state.keyRequests) {
        if (entry.number == number) {
            if (int32_t(now - entry.at) < 10000) return;
            slot = &entry; break;
        }
        if (!entry.number || int32_t(entry.at - slot->at) < 0) slot = &entry;
    }
    if (queueNodeInfo(number, true)) {
        slot->number = number; slot->at = now;
        Serial.printf("Meshtastic: requesting NodeInfo from=%08lx\n", (unsigned long)number);
    }
}
size_t findNode(uint32_t number) {
    for (size_t i = 0; i < state.nodeCount; ++i) if (state.nodes[i].number == number) return i;
    if (state.nodeCount == 32) return 32; // never invalidate indices selected by the UI
    const size_t i = state.nodeCount++; state.nodes[i].number = number;
    snprintf(state.nodes[i].peer.id, sizeof(state.nodes[i].peer.id), "!%08lx", (unsigned long)number);
    snprintf(state.nodes[i].peer.name, sizeof(state.nodes[i].peer.name), "!%08lx", (unsigned long)number);
    return i;
}
bool sendResponse(const MeshRxResult &result, MeshPortNum port, const uint8_t *payload, size_t size, bool pki) {
    uint8_t data[239], encrypted[239], frame[255];
    size_t length = buildResponseData(port, payload, size, state.session.node_num,
        result.packet.from, result.packet.id, data, sizeof(data));
    if (!length) return false;
    const uint32_t id = state.session.id_gen.next(); uint8_t hash = 0;
    if (pki) {
        const auto *key = state.session.node_keys.getKey(result.packet.from);
        if (!key || length + MESH_PKI_OVERHEAD > sizeof(encrypted)) return false;
        length = meshPkiEncrypt(state.session.pki.private_key, key, state.session.node_num,
            id, data, length, encrypted, hardwareRandom);
        if (!length) return false;
    } else {
        const int channelIndex = result.channel_idx >= 0 ? result.channel_idx : 0;
        const auto &channel = state.session.channels.channels[channelIndex]; hash = channel.hash;
        memcpy(encrypted, data, length);
        if (!meshEncrypt(&channel.key, state.session.node_num, id, encrypted, length)) return false;
    }
    const uint8_t hops = result.packet.hop_start ? result.packet.hop_start : 3;
    length = meshBuildPacket(frame, result.packet.from, state.session.node_num, id, hops, hops,
        false, false, hash, 0, uint8_t(state.session.node_num), encrypted, length);
    const bool ok = enqueue(frame, length);
    Serial.printf("Meshtastic: response port=%u request=%08lx to=%08lx queued=%u\n", unsigned(port),
        (unsigned long)result.packet.id, (unsigned long)result.packet.from, unsigned(ok));
    return ok;
}
void respondToRequest(const MeshRxResult &result) {
    if (result.packet.to != state.session.node_num) return;
    if (result.data.portnum == PORT_TRACEROUTE && result.data.want_response && !result.data.request_id) {
        uint8_t route[220];
        const size_t size = buildTraceRoutePayload(result.data.payload, result.data.payload_len,
            result.packet.snr, route, sizeof(route));
        if (size && sendResponse(result, PORT_TRACEROUTE, route, size, result.is_pki)) return;
    }
    if (result.packet.want_ack) {
        const uint8_t routing[2] = {0x18, 0};
        sendResponse(result, PORT_ROUTING, routing, sizeof(routing), false);
    }
}
}
MeshtasticService meshtasticService;
void MeshtasticService::loadConfiguration() {
    if (state.loaded) return;
    Preferences p; p.begin("meshtastic", true); Config saved{};
    if (p.getBytes("config", &saved, sizeof(saved)) == sizeof(saved) && validConfig(saved)) state.config = saved;
    const uint8_t role = p.getUChar("role", ROLE_CLIENT_MUTE);
    if (role <= ROLE_CLIENT_MUTE) state.role = role;
    p.end(); state.loaded = true;
}
bool MeshtasticService::start() {
    loadConfiguration(); state.error[0] = 0;
    if (!state.initialized) {
        uint8_t mac[6]{}; esp_efuse_mac_get_default(mac);
        uint32_t number = uint32_t(mac[2]) << 24 | uint32_t(mac[3]) << 16 | uint32_t(mac[4]) << 8 | mac[5];
        if (number < 4 || number == 0xffffffff) number ^= 0x57494f00;
        state.session.init(MeshRegion(state.config.region), MeshModemPreset(state.config.preset), MeshRole(state.role), number);
        uint8_t identity[64]; Preferences p; p.begin("meshtastic", false);
        if (p.getBytes("identity", identity, sizeof(identity)) == sizeof(identity)) {
            state.session.pki.load(identity, identity + 32);
        } else {
            if (!state.session.pki.generate()) { p.end(); failure("PKI identity generation failed", -1); return false; }
            memcpy(identity, state.session.pki.public_key, 32); memcpy(identity + 32, state.session.pki.private_key, 32);
            if (p.putBytes("identity", identity, sizeof(identity)) != sizeof(identity)) { p.end(); failure("PKI identity save failed", -1); return false; }
        }
        p.end(); memset(identity, 0, sizeof(identity));
        state.session.id_gen.random_base = hardwareRandom() & 0xfffffc00;
        restoreNodes();
        for (size_t i = 0; i < chatStore.peerCount(); ++i) {
            const auto *saved = chatStore.peer(i); unsigned number = 0; char extra = 0;
            if (strcmp(saved->protocol, "meshtastic") || strlen(saved->peer.id) != 9 || sscanf(saved->peer.id, "!%8x%c", &number, &extra) != 1 ||
                number < 4 || number == nodeNumber() || number == MESH_ADDR_BROADCAST) continue;
            const size_t index = findNode(number); if (index >= 32) continue;
            auto &peer = state.nodes[index].peer;
            snprintf(peer.name, sizeof(peer.name), "%s", saved->peer.name); peer.contact = saved->peer.contact;
            uint8_t present = 0; for (uint8_t byte : saved->publicKey) present |= byte;
            if (present) state.session.node_keys.setKey(number, saved->publicKey);
            peer.hasPublicKey = state.session.node_keys.getKey(number) != nullptr;
        }
        state.initialized = true;
    }
    state.signalKnown = false; state.rssi = 0; state.snr = 0;
    setupSessionConfig(); const int code = configureRadio();
    if (code) { wioRadio().standby(); wioRadio().sleep(false); failure("Meshtastic radio start failed", code); return false; }
    state.online = true; state.transmitting = false; state.count = 0; state.nextAdvert = millis() + 5000;
    const auto rc = preset(presetIndex());
    Serial.printf("Meshtastic: ready !%08lx %s/%s %.3f MHz BW%.1f SF%u CR%u\n", (unsigned long)nodeNumber(),
        regionName(regionIndex()), rc.name, rc.frequency, rc.bandwidth, rc.spreadingFactor, rc.codingRate);
    return true;
}
bool MeshtasticService::stop() {
    if (!state.online) return true;
    auto &radio = wioRadio(); int code = radio.standby();
    if (code) { failure("Meshtastic stop failed", code); return false; }
    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL); code = radio.sleep(false);
    if (code) { radio.startReceive(); failure("Meshtastic sleep failed", code); return false; }
    state.online = false; state.transmitting = false; state.count = 0;
    for (auto &pending : state.pending) pending.length = 0;
    for (auto &relay : state.relays) relay.tx.length = 0;
    return true;
}
bool MeshtasticService::ready() const { return state.online; }
void MeshtasticService::setEvents(MeshEvents *events) { state.events = events; }
size_t MeshtasticService::peerCount() const { return state.nodeCount; }
CommunicationPeer MeshtasticService::peer(size_t index) const { return index < state.nodeCount ? state.nodes[index].peer : CommunicationPeer{}; }
bool MeshtasticService::setContact(size_t index, bool enabled) {
    if (index >= state.nodeCount) return false;
    const bool previous = state.nodes[index].peer.contact;
    state.nodes[index].peer.contact = enabled;
    if (!saveNodes()) { state.nodes[index].peer.contact = previous; failure("Could not save contacts", -1); return false; }
    if (state.events) state.events->onNodeChanged(); return true;
}
const char *MeshtasticService::nodeName() const { return state.config.name; }
bool MeshtasticService::setNodeName(const char *name) {
    if (!name || !*name || strlen(name) > 24) return false;
    auto c = state.config; snprintf(c.name, sizeof(c.name), "%s", name); return applyConfig(c);
}
size_t MeshtasticService::presetCount() const { return MODEM_PRESET_COUNT; }
size_t MeshtasticService::presetIndex() const { return state.config.preset; }
RadioPreset MeshtasticService::preset(size_t index) const {
    if (index >= MODEM_PRESET_COUNT) index = state.config.preset;
    auto rc = meshBuildRadioConfig(MeshRegion(state.config.region), MeshModemPreset(index), state.config.channel[0] ? state.config.channel : meshPresetName(MeshModemPreset(index)), 19);
    return {meshPresetName(MeshModemPreset(index)), state.config.frequency ? state.config.frequency : rc.frequency_mhz,
        rc.bandwidth_khz, rc.spreading_factor, rc.coding_rate};
}
bool MeshtasticService::selectPreset(size_t index) { if (index >= presetCount()) return false; auto c = state.config; c.preset = index; return applyConfig(c); }
size_t MeshtasticService::roleIndex() const { return state.role; }
const char *MeshtasticService::roleName(size_t index) const { return index == 0 ? "Client" : "Client Mute"; }
bool MeshtasticService::selectRole(size_t index) {
    if (index > ROLE_CLIENT_MUTE) { failure("Unsupported device role", -1); return false; }
    if (state.count || state.transmitting) { failure("Wait for TX queue to finish", -1); return false; }
    Preferences p; if (!p.begin("meshtastic", false)) { failure("Could not save role", -1); return false; }
    const bool saved = p.putUChar("role", index) == 1; p.end();
    if (!saved) { failure("Could not save role", -1); return false; }
    state.role = index; state.session.role = MeshRole(index);
    for (auto &relay : state.relays) relay.tx.length = 0;
    memset(state.relaySeen, 0, sizeof(state.relaySeen));
    state.error[0] = 0; if (state.online) state.nextAdvert = millis() + 1000;
    return true;
}
bool MeshtasticService::signalKnown() const { return ready() && state.signalKnown; }
size_t MeshtasticService::regionCount() const { return REGION_UNSET; }
const char *MeshtasticService::regionName(size_t index) const { return meshGetRegion(MeshRegion(index < regionCount() ? index : state.config.region))->name; }
size_t MeshtasticService::regionIndex() const { return state.config.region; }
bool MeshtasticService::selectRegion(size_t index) { if (index >= regionCount()) return false; auto c = state.config; c.region = index; c.frequency = 0; return applyConfig(c); }
bool MeshtasticService::setFrequency(float mhz) { auto c = state.config; c.frequency = mhz; return applyConfig(c); }
bool MeshtasticService::setChannel(const char *name, const char *base64Key) {
    if (!name || strlen(name) >= sizeof(state.config.channel) || !base64Key || strlen(base64Key) > 44) return false;
    auto c = state.config; size_t length = 0;
    if (mbedtls_base64_decode(c.psk, sizeof(c.psk), &length, (const unsigned char *)base64Key, strlen(base64Key))) return false;
    c.pskLength = length; snprintf(c.channel, sizeof(c.channel), "%s", name); return applyConfig(c);
}
const char *MeshtasticService::channelName() const { return state.config.channel[0] ? state.config.channel : meshPresetName(MeshModemPreset(state.config.preset)); }
uint32_t MeshtasticService::nodeNumber() const { return state.session.node_num; }
bool MeshtasticService::importPublicKey(uint32_t number, const uint8_t key[32]) {
    if (!ready() || number < 4 || number == nodeNumber() || number == MESH_ADDR_BROADCAST) return false;
    uint8_t combined = 0; for (unsigned i = 0; i < 32; ++i) combined |= key[i]; if (!combined) return false;
    const size_t index = findNode(number); if (index >= 32) return false;
    state.session.node_keys.setKey(number, key); state.nodes[index].peer.hasPublicKey = true;
    if (!saveNodes()) { failure("Could not save peer public key", -1); return false; }
    state.error[0] = 0; if (state.events) state.events->onNodeChanged();
    for (auto &pending : state.pending) if (pending.length) {
        MeshRxPacket header{};
        if (meshParsePacket(pending.frame, pending.length, &header) && header.from == number) {
            const auto copy = pending; pending.length = 0;
            if (int32_t(millis() - copy.expires) < 0) receiveFrame(copy.frame, copy.length, copy.rssi, copy.snr);
        }
    }
    return true;
}
bool MeshtasticService::forgetPeer(uint32_t number) {
    size_t index = 0; while (index < state.nodeCount && state.nodes[index].number != number) ++index;
    if (index == state.nodeCount) return false;
    const auto removed = state.nodes[index]; --state.nodeCount;
    memmove(state.nodes + index, state.nodes + index + 1, (state.nodeCount - index) * sizeof(Node));
    if (!saveNodes()) {
        memmove(state.nodes + index + 1, state.nodes + index, (state.nodeCount - index) * sizeof(Node));
        state.nodes[index] = removed; ++state.nodeCount; return false;
    }
    auto &keys = state.session.node_keys;
    for (size_t i = 0; i < keys.count; ++i) if (keys.entries[i].node_num == number) {
        --keys.count; memmove(keys.entries + i, keys.entries + i + 1, (keys.count - i) * sizeof(MeshNodeKey)); break;
    }
    for (auto &pending : state.pending) if (pending.length) {
        MeshRxPacket packet{};
        if (meshParsePacket(pending.frame, pending.length, &packet) && packet.from == number) pending.length = 0;
    }
    char id[12]; snprintf(id, sizeof(id), "!%08lx", (unsigned long)number);
    chatStore.forgetPeer("meshtastic", id);
    if (state.events) state.events->onNodeChanged(); return true;
}
int16_t MeshtasticService::rssi() const { return state.rssi; }
float MeshtasticService::snr() const { return state.snr; }
const char *MeshtasticService::error() const { return state.error; }
uint32_t MeshtasticService::txCount() const { return state.tx; }
uint32_t MeshtasticService::rxCount() const { return state.rx; }
uint32_t MeshtasticService::rejectedCount() const { return state.rejected; }
uint32_t MeshtasticService::lastTxId() const { return state.txId; }
uint32_t MeshtasticService::lastAckId() const { return state.ackId; }
unsigned MeshtasticService::queuedCount() const { return state.count; }
bool MeshtasticService::advertise() {
    return ready() && queueNodeInfo(MESH_ADDR_BROADCAST, state.tx == 0);
}
bool MeshtasticService::peerPublicKey(size_t index, uint8_t output[32]) const {
    if (index >= state.nodeCount) return false;
    const auto *key = state.session.node_keys.getKey(state.nodes[index].number);
    if (!key) return false; memcpy(output, key, 32); return true;
}
bool MeshtasticService::publicKey(uint8_t output[32]) const {
    if (!state.initialized) return false;
    memcpy(output, state.session.pki.public_key, 32); return true;
}

bool MeshtasticService::sendPublic(const char *text) {
    if (!text || !*text || strlen(text) > 220 || !ready()) { failure("Text must contain 1-220 bytes", -1); return false; }
    uint8_t frame[255]; size_t length = state.session.buildTextTx(0, MESH_ADDR_BROADCAST, text, false, false, frame);
    if (!enqueue(frame, length)) return false;
    if (state.events) state.events->onMeshMessage(text, systemClock.now(), false, true, false, nodeName()); return true;
}
bool MeshtasticService::sendDirect(size_t index, const char *text) {
    if (!text || !*text || strlen(text) > 220 || !ready() || index >= state.nodeCount) return false;
    const auto &node = state.nodes[index];
    if (!state.session.node_keys.getKey(node.number)) { requestKey(node.number); failure("Requesting recipient public key; retry after NodeInfo", -1); return false; }
    uint8_t frame[255]; size_t length = state.session.buildTextDm(node.number, text, true, frame, hardwareRandom);
    if (!enqueue(frame, length)) return false;
    if (state.events) state.events->onMeshMessage(text, systemClock.now(), true, true, node.peer.contact, node.peer.name, node.peer.id); return true;
}
bool MeshtasticService::receiveFrame(const uint8_t *frame, size_t length, float rssiValue, float snrValue) {
    if (!ready() || length <= 16 || length > 255) return false;
    MeshRxPacket header{};
    if (!meshParsePacket(frame, length, &header) || !header.from || header.from == MESH_ADDR_BROADCAST ||
        header.from == nodeNumber()) return false;
    // RF strength is meaningful even for duplicate or not-yet-decryptable packets.
    state.rssi = rssiValue; state.snr = snrValue; state.signalKnown = true;
    const bool relaySeen = relayDuplicate(header);
    if (header.channel_hash == 0 && header.to != MESH_ADDR_BROADCAST && header.to != nodeNumber() &&
        header.payload_len >= MESH_PKI_OVERHEAD && state.session.channels.channels[0].hash != 0) {
        if (!relaySeen) scheduleRelay(frame, length, header, snrValue);
        return false; // An opaque private relay is not a local decrypt rejection.
    }
    if (meshIsPkiCandidate(&header, nodeNumber()) && !state.session.node_keys.getKey(header.from)) {
        for (auto &pending : state.pending) {
            MeshRxPacket old{};
            if (pending.length && meshParsePacket(pending.frame, pending.length, &old) && old.from == header.from && old.id == header.id) {
                requestKey(header.from); return false;
            }
        }
        for (auto &pending : state.pending) if (!pending.length || int32_t(millis() - pending.expires) >= 0) {
            memcpy(pending.frame, frame, length); pending.length = length; pending.expires = millis() + 45000;
            pending.rssi = rssiValue; pending.snr = snrValue; break;
        }
        failure("Private message waiting for sender public key", -1);
        Serial.printf("Meshtastic: PKI waiting for key from=%08lx id=%08lx\n", (unsigned long)header.from, (unsigned long)header.id);
        requestKey(header.from); return false;
    }
    uint8_t previousKey[32]{}; const auto *known = state.session.node_keys.getKey(header.from);
    const bool hadKey = known != nullptr; if (known) memcpy(previousKey, known, 32);
    MeshRxResult result{};
    if (!state.session.processRx(frame, length, rssiValue, snrValue, &result)) {
        ++state.rejected;
        Serial.printf("Meshtastic: decrypt rejected from=%08lx to=%08lx hash=%u key=%u\n",
            (unsigned long)header.from, (unsigned long)header.to, unsigned(header.channel_hash), unsigned(hadKey));
        if (meshIsPkiCandidate(&header, nodeNumber())) { failure("Private message authentication failed", -1); requestKey(header.from); }
        return false;
    }
    if (!result.packet.from || result.packet.from == MESH_ADDR_BROADCAST || result.packet.from == nodeNumber()) return false;
    if (!relaySeen) scheduleRelay(frame, length, header, snrValue);
    if (result.packet.to != MESH_ADDR_BROADCAST && result.packet.to != nodeNumber() && result.data.portnum != PORT_NODEINFO) return false;
    for (const auto &seen : state.seen) if (seen.from == result.packet.from && seen.id == result.packet.id) {
        respondToRequest(result); // A retry can mean our earlier response was lost.
        return false;
    }
    state.seen[state.seenAt++ % 64] = {result.packet.from, result.packet.id};
    ++state.rx; state.rssi = rssiValue; state.snr = snrValue;
    Serial.printf("Meshtastic: RX port=%u from=%08lx to=%08lx id=%08lx ack=%u response=%u pki=%u\n",
        unsigned(result.data.portnum), (unsigned long)result.packet.from, (unsigned long)result.packet.to,
        (unsigned long)result.packet.id, unsigned(result.packet.want_ack), unsigned(result.data.want_response), unsigned(result.is_pki));
    const size_t beforeCount = state.nodeCount;
    const size_t index = findNode(result.packet.from);
    if (index < 32) {
        auto &peer = state.nodes[index].peer; const auto previousPeer = peer; peer.lastSeen = systemClock.now(); peer.snrQuarter = int16_t(snrValue * 4);
        if (result.data.portnum == PORT_NODEINFO) {
            MeshUser user{};
            if (meshDecodeUser(result.data.payload, result.data.payload_len, &user)) {
                if (user.long_name[0]) snprintf(peer.name, sizeof(peer.name), "%s", user.long_name);
                peer.hasPublicKey = state.session.node_keys.getKey(result.packet.from) != nullptr;
            }
        }
        const auto *newKey = state.session.node_keys.getKey(result.packet.from);
        if (state.nodeCount != beforeCount || strcmp(previousPeer.name, peer.name) ||
            (newKey && (!hadKey || memcmp(previousKey, newKey, 32)))) {
            if (!saveNodes()) failure("Could not save peer keys", -1);
        }
        if (state.events) state.events->onNodeChanged();
    }
    respondToRequest(result);
    if (result.data.portnum == PORT_NODEINFO && result.data.want_response && (result.packet.to == nodeNumber() || result.packet.to == MESH_ADDR_BROADCAST)) queueNodeInfo(result.packet.from, false);
    if (result.data.portnum == PORT_NODEINFO && state.session.node_keys.getKey(result.packet.from)) {
        state.error[0] = 0;
        for (auto &pending : state.pending) if (pending.length) {
            MeshRxPacket stored{};
            if (meshParsePacket(pending.frame, pending.length, &stored) && stored.from == result.packet.from) {
                const auto copy = pending; pending.length = 0;
                if (int32_t(millis() - copy.expires) < 0) receiveFrame(copy.frame, copy.length, copy.rssi, copy.snr);
            }
        }
    }
    if (result.data.portnum == PORT_ROUTING && result.data.request_id) {
        auto cursor = pbCursor(result.data.payload, result.data.payload_len); bool valid = true; uint64_t error = 0;
        while (!cursor.exhausted()) {
            uint32_t field; uint8_t wire;
            if (!cursor.readTag(&field, &wire)) { valid = false; break; }
            if (field == 3 && wire == 0) { if (!cursor.readVarint(&error)) { valid = false; break; } }
            else if (!cursor.skipField(wire)) { valid = false; break; }
        }
        if (valid && !error) state.ackId = result.data.request_id;
    }
    if (result.data.portnum != PORT_TEXT_MESSAGE && state.events &&
        (result.packet.to == nodeNumber() || result.packet.to == MESH_ADDR_BROADCAST)) {
        const auto peer = index < 32 ? state.nodes[index].peer : CommunicationPeer{};
        char sender[12]; snprintf(sender, sizeof(sender), "!%08lx", (unsigned long)result.packet.from);
        state.events->onMeshData(peer.name[0] ? peer.name : sender, peer.id[0] ? peer.id : sender,
            systemClock.now(), uint16_t(result.data.portnum), result.data.payload, result.data.payload_len,
            result.packet.to == nodeNumber(), result.packet.id);
    }
    if (result.data.portnum == PORT_TEXT_MESSAGE) {
        char text[241]{}; memcpy(text, result.data.payload, result.data.payload_len);
        char sender[12]; snprintf(sender, sizeof(sender), "!%08lx", (unsigned long)result.packet.from);
        const auto peer = index < 32 ? state.nodes[index].peer : CommunicationPeer{};
        if (state.events) state.events->onMeshMessage(text, systemClock.now(), result.packet.to == nodeNumber(), false, peer.contact, peer.name[0] ? peer.name : sender, peer.id[0] ? peer.id : sender, result.packet.id);
        Serial.printf("Meshtastic: RX from=%08lx id=%08lx bytes=%u\n", (unsigned long)result.packet.from, (unsigned long)result.packet.id, unsigned(result.data.payload_len));
    }
    return true;
}
void MeshtasticService::poll() {
    if (!ready() || int32_t(millis() - state.nextPoll) < 0) return;
    const uint32_t now = millis(); state.nextPoll = now + 10; auto &radio = wioRadio(); const auto irq = radio.getIrqFlags();
    if (state.transmitting) {
        if ((irq & RADIOLIB_SX126X_IRQ_TX_DONE) || int32_t(now - state.txDeadline) >= 0) {
            const bool done = irq & RADIOLIB_SX126X_IRQ_TX_DONE; radio.finishTransmit();
            auto &tx = state.queue[state.head];
            if (done) {
                ++state.tx; memcpy(&state.txId, tx.frame + 8, 4);
                Serial.printf("Meshtastic: TX done id=%08lx bytes=%u\n", (unsigned long)state.txId, unsigned(tx.length));
                if (tx.advert && state.events) state.events->onAdvertTransmitted(false);
            } else failure("TX timeout", -1);
            state.head = (state.head + 1) % 4; --state.count; state.transmitting = false;
            radio.startReceive(); state.due = now + (hardwareRandom() % 8) * meshSlotTimeMs(state.session.preset);
        }
        return;
    }
    if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
        const size_t length = radio.getPacketLength(); uint8_t frame[255];
        if (length <= sizeof(frame) && radio.readData(frame, length) == RADIOLIB_ERR_NONE) {
            // Read metadata after draining RX, in one transaction. RssiPkt is
            // packet-average power; RadioLib getRSSI() selects SignalRssiPkt.
            const uint32_t status = radio.getPacketStatus();
            const float rssiValue = -float((status >> 16) & 0xff) / 2;
            const float snrValue = float(int8_t((status >> 8) & 0xff)) / 4;
            state.rssi = rssiValue; state.snr = snrValue; state.signalKnown = status != 0;
            receiveFrame(frame, length, rssiValue, snrValue);
            if (!status) state.signalKnown = false;
            Serial.printf("Meshtastic: RF RSSI=%.1f SNR=%.2f status=%06lx\n", rssiValue, snrValue, (unsigned long)status);
        } else ++state.rejected;
        radio.startReceive();
    } else if (irq & (RADIOLIB_SX126X_IRQ_TIMEOUT | RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR)) radio.startReceive();
    for (auto &relay : state.relays) if (relay.tx.length && int32_t(now - relay.due) >= 0 && state.count < 3) {
        if (enqueue(relay.tx.frame, relay.tx.length, false, true)) relay.tx.length = 0;
    }
    if (state.count && int32_t(now - state.due) >= 0) {
        if (radio.isReceiving()) return;
        const int cad = radio.scanChannel();
        if (cad == RADIOLIB_LORA_DETECTED) { radio.startReceive(); state.due = now + 100 + hardwareRandom() % 1500; }
        else if (cad == RADIOLIB_CHANNEL_FREE) {
            auto &tx = state.queue[state.head]; const int code = radio.startTransmit(tx.frame, tx.length);
            if (!code) { state.transmitting = true; state.txDeadline = now + radio.getTimeOnAir(tx.length) / 1000 + 2000; }
            else { failure("TX start failed", code); state.head = (state.head + 1) % 4; --state.count; radio.startReceive(); }
        } else { failure("Channel scan failed", cad); radio.startReceive(); state.due = now + 1000; }
    }
    for (auto &pending : state.pending) if (pending.length) {
        if (int32_t(now - pending.expires) >= 0) { pending.length = 0; ++state.rejected; failure("Sender public key request timed out", -1); }
        else { MeshRxPacket header{}; if (meshParsePacket(pending.frame, pending.length, &header)) requestKey(header.from); }
    }
    if (int32_t(now - state.nextAdvert) >= 0 && state.count < 4) { advertise(); state.nextAdvert = now + 15UL * 60 * 1000; }
}
