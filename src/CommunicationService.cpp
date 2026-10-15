#include "CommunicationService.h"
#include "MeshService.h"
#include "MeshtasticService.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>

namespace {
class MeshCoreBackend final : public CommunicationBackend {
    bool initialized_ = false;
public:
    const char *name() const override { return "meshcore"; }
    void setEvents(MeshEvents *events) override { meshService.setEvents(events); }
    size_t peerCount() const override { return meshService.nodeCount(); }
    CommunicationPeer peer(size_t index) const override {
        CommunicationPeer result{}; if (index >= peerCount()) return result;
        const auto &node = meshService.nodes()[index];
        snprintf(result.name, sizeof(result.name), "%s", node.name);
        for (unsigned i = 0; i < 32; ++i) snprintf(result.id + i * 2, 3, "%02x", node.id.pub_key[i]);
        result.lastSeen = node.lastSeen; result.snrQuarter = node.snrQuarter;
        result.contact = node.contact; result.hasPublicKey = true; return result;
    }
    bool setContact(size_t index, bool value) override { return meshService.setContact(index, value); }
    const char *nodeName() const override { return meshService.nodeName(); }
    bool setNodeName(const char *name) override { return meshService.setNodeName(name); }
    bool advertise() override { return meshService.advertise(); }
    size_t presetCount() const override { return MeshService::radioPresetCount(); }
    size_t presetIndex() const override { return meshService.radioPresetIndex(); }
    RadioPreset preset(size_t index) const override { return MeshService::radioPreset(index); }
    bool selectPreset(size_t index) override { return meshService.selectRadioPreset(index); }
    int16_t rssi() const override { return meshService.lastRssi(); }
    float snr() const override { return meshService.lastSnr(); }
    bool start() override {
        if (!initialized_) {
            if (!meshService.beginService()) return false;
            initialized_ = true;
        }
        return meshService.setLoRaEnabled(true);
    }
    bool stop() override { return meshService.setLoRaEnabled(false); }
    void poll() override { meshService.loopService(); }
    bool ready() const override { return meshService.ready(); }
    bool sendPublic(const char *text) override { return meshService.sendPublic(text); }
    bool sendDirect(size_t peer, const char *text) override { return meshService.sendDirect(peer, text); }
} meshCore;
}
CommunicationService communication;
bool CommunicationService::begin() {
    Preferences prefs; prefs.begin("communication", true);
    const String selected = prefs.getString("protocol", "meshcore");
    const String config = prefs.getString("configured", "meshcore"); prefs.end();
    configured_ = config == "meshtastic" ? static_cast<CommunicationBackend *>(&meshtasticService) : &meshCore;
    return select(selected.c_str());
}
const char *CommunicationService::protocol() const { return active_ ? active_->name() : "off"; }
bool CommunicationService::ready() const { return active_ && active_->ready(); }
void CommunicationService::poll() { if (active_) active_->poll(); }
bool CommunicationService::sendPublic(const char *text) { error_ = ""; return ready() && active_->sendPublic(text); }
bool CommunicationService::sendDirect(size_t peer, const char *text) { error_ = ""; return ready() && active_->sendDirect(peer, text); }
bool CommunicationService::select(const char *name) {
    error_ = "";
    if (!name || (strcmp(name, "off") && strcmp(name, "meshcore") && strcmp(name, "meshtastic"))) {
        error_ = "Protocol backend not installed"; return false;
    }
    CommunicationBackend *next = !strcmp(name, "meshcore") ? static_cast<CommunicationBackend *>(&meshCore) :
        !strcmp(name, "meshtastic") ? static_cast<CommunicationBackend *>(&meshtasticService) : nullptr;
    if (next == active_) return true;
    CommunicationBackend *previous = active_;
    if (previous && !previous->stop()) { error_ = "Cannot stop active backend"; return false; }
    active_ = nullptr;
    if (next && !next->start()) {
        if (previous && previous->start()) active_ = previous;
        error_ = "Cannot start backend"; return false;
    }
    active_ = next;
    if (next) configured_ = next;
    Preferences prefs; prefs.begin("communication", false);
    prefs.putString("protocol", protocol());
    prefs.putString("configured", configurationProtocol()); prefs.end();
    if (events_) { events_->onNodeChanged(); events_->onRadioState(ready()); }
    return true;
}

const char *CommunicationService::configurationProtocol() const { return configured_ ? configured_->name() : "meshcore"; }
const char *CommunicationService::error() const { return *error_ ? error_ : active_ ? active_->error() : ""; }
void CommunicationService::setEvents(MeshEvents *events) { events_ = events; meshCore.setEvents(events); meshtasticService.setEvents(events); }
size_t CommunicationService::peerCount() const { return configured_ ? configured_->peerCount() : 0; }
CommunicationPeer CommunicationService::peer(size_t index) const { return configured_ ? configured_->peer(index) : CommunicationPeer{}; }
bool CommunicationService::setContact(size_t index, bool enabled) { return configured_ && configured_->setContact(index, enabled); }
const char *CommunicationService::nodeName() const { return configured_ ? configured_->nodeName() : meshService.nodeName(); }
bool CommunicationService::setNodeName(const char *name) { return configured_ && configured_->setNodeName(name); }
bool CommunicationService::advertise() { return ready() && active_->advertise(); }
size_t CommunicationService::radioPresetCount() const { return configured_ ? configured_->presetCount() : MeshService::radioPresetCount(); }
size_t CommunicationService::radioPresetIndex() const { return configured_ ? configured_->presetIndex() : meshService.radioPresetIndex(); }
RadioPreset CommunicationService::radioPreset(size_t index) const { return configured_ ? configured_->preset(index) : MeshService::radioPreset(index); }
bool CommunicationService::selectRadioPreset(size_t index) { return configured_ && configured_->selectPreset(index); }
int16_t CommunicationService::rssi() const { return active_ ? active_->rssi() : 0; }
float CommunicationService::snr() const { return active_ ? active_->snr() : 0; }

bool CommunicationService::signalKnown() const { return ready() && active_->signalKnown(); }
