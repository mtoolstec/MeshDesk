#include "ChatStore.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <Arduino.h>
#endif
namespace {
void *allocate(size_t size) {
#ifdef ARDUINO
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}
class JsonAllocator : public ArduinoJson::Allocator {
    void *allocate(size_t size) override { return ::allocate(size); }
    void deallocate(void *pointer) override { free(pointer); }
    void *reallocate(void *pointer, size_t size) override {
#ifdef ARDUINO
        return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        return realloc(pointer, size);
#endif
    }
} jsonAllocator;
uint32_t crc32(const void *data, size_t size, uint32_t crc = 0xffffffff) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}
uint32_t hash(const char *s) { return s ? crc32(s, strlen(s)) : 0; }
bool terminated(const char *s, size_t size) { return memchr(s, 0, size); }
bool valid(const ChatRecord &record) {
    return record.magic == 0x31434857 && record.id && (record.kind == 1 || record.kind == 2) &&
        record.length <= sizeof(record.payload) && record.flags <= 7 &&
        terminated(record.protocol, sizeof(record.protocol)) && terminated(record.peerId, sizeof(record.peerId)) &&
        terminated(record.peer, sizeof(record.peer)) && record.crc == crc32(reinterpret_cast<const uint8_t *>(&record) + 8, 504);
}
bool directory(const char *path) {
    struct stat info{};
    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
    return mkdir(path, 0775) == 0;
}
bool durableClose(FILE *file) {
    const bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    return fclose(file) == 0 && ok;
}
bool protocolValid(const char *s) { return s && (!strcmp(s, "meshcore") || !strcmp(s, "meshtastic")); }
uint32_t peerChecksum(const StoredPeer *peers, size_t count) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < count; ++i) {
        const auto &p = peers[i];
        crc = crc32(p.protocol, strlen(p.protocol), crc);
        crc = crc32(p.peer.id, strlen(p.peer.id), crc);
        crc = crc32(p.peer.name, strlen(p.peer.name), crc);
        const uint8_t flags = p.peer.contact; crc = crc32(&flags, 1, crc);
        crc = crc32(p.publicKey, sizeof(p.publicKey), crc);
    }
    return crc;
}
}
ChatStore chatStore;
ChatStore::ChatStore(const char *root) { snprintf(root_, sizeof(root_), "%s", root); }
ChatStore::~ChatStore() { free(index_); free(peers_); }
void ChatStore::fail(const char *message) { snprintf(error_, sizeof(error_), "%s", message); }
void ChatStore::segmentPath(uint32_t segment, char *path, size_t size) const {
    snprintf(path, size, "%s/messages/%08lu.log", root_, (unsigned long)segment);
}
bool ChatStore::begin() {
    if (initialized_) return true;
    // The SD mount exists before this call; create parents without touching /apps.
    char path[192]; snprintf(path, sizeof(path), "%s", root_);
    for (char *p = path + 1; *p; ++p) if (*p == '/') { *p = 0; if (!directory(path)) { fail("Data directory unavailable"); return false; } *p = '/'; }
    if (!directory(path)) { fail("Cannot create chat directory"); return false; }
    snprintf(path, sizeof(path), "%s/messages", root_);
    if (!directory(path)) { fail("Cannot create message directory"); return false; }
    if (!index_) index_ = static_cast<Index *>(allocate(sizeof(Index) * SEGMENTS * SLOTS));
    if (!peers_) peers_ = static_cast<StoredPeer *>(allocate(sizeof(StoredPeer) * PEERS));
    if (!index_ || !peers_) { fail("Chat index allocation failed"); return false; }
    indexCount_ = peerCount_ = segmentCount_ = generation_ = 0;
    DIR *directory = opendir(path);
    if (!directory) { fail("Cannot scan message directory"); return false; }
    while (auto *entry = readdir(directory)) {
        unsigned number = 0; char extra = 0;
        if (strlen(entry->d_name) != 12 || strspn(entry->d_name, "0123456789") != 8 || sscanf(entry->d_name, "%8u.log%c", &number, &extra) != 1 || !number) continue;
        unsigned at = 0; while (at < segmentCount_ && segments_[at] < number) ++at;
        if (segmentCount_ == SEGMENTS) {
            if (!at) { char old[192]; segmentPath(number, old, sizeof(old)); unlink(old); continue; }
            char old[192]; segmentPath(segments_[0], old, sizeof(old)); unlink(old);
            memmove(segments_, segments_ + 1, (--segmentCount_) * sizeof(uint32_t)); --at;
        }
        memmove(segments_ + at + 1, segments_ + at, (segmentCount_ - at) * sizeof(uint32_t));
        segments_[at] = number; ++segmentCount_;
    }
    closedir(directory);
    uint32_t diskLast = 0;
    for (unsigned i = 0; i < segmentCount_; ++i) {
        segmentPath(segments_[i], path, sizeof(path)); FILE *file = fopen(path, "rb");
        if (!file) { fail("Cannot read message segment"); return false; }
        ChatRecord record{}; unsigned slot = 0;
        while (slot < SLOTS && fread(&record, 1, sizeof(record), file) == sizeof(record)) {
            if (valid(record)) { if (record.id > diskLast) diskLast = record.id; apply(record, segments_[i], slot); }
            ++slot;
        }
        const bool ok = !ferror(file);
#ifdef ARDUINO
        if (!ok) Serial.printf("Chats: read failed path=%s slot=%u offset=%ld errno=%d\n", path, slot, ftell(file), errno);
#endif
        fclose(file);
        if (!ok) { fail("Message read failed"); return false; }
    }
    current_ = segmentCount_ ? segments_[segmentCount_ - 1] : 1;
    for (unsigned i = 0; i < count_; ++i) queue_[(head_ + i) % QUEUE].id = ++diskLast;
    lastId_ = diskLast;
    snprintf(path, sizeof(path), "%s/contacts.json", root_); loadPeers(path, true);
    snprintf(path, sizeof(path), "%s/contacts.backup.json", root_); loadPeers(path, true);
    initialized_ = true; ++revision_; error_[0] = 0; return true;
}
void ChatStore::apply(const ChatRecord &record, uint32_t segment, unsigned slot) {
    if (record.kind == 2) {
        for (size_t i = 0; i < indexCount_; ++i) if (index_[i].id == record.targetId) {
            memmove(index_ + i, index_ + i + 1, (--indexCount_ - i) * sizeof(Index)); break;
        }
        return;
    }
    for (size_t i = 0; i < indexCount_; ++i) if (index_[i].id == record.id) return;
    if (indexCount_ == SEGMENTS * SLOTS) return;
    index_[indexCount_++] = {record.id, segment, hash(record.protocol), hash(record.peerId), uint16_t(slot), record.port, record.flags};
}
uint32_t ChatStore::append(const char *protocol, const char *peerId, const char *peer, uint32_t timestamp,
                          uint16_t port, const uint8_t *payload, size_t length, bool direct, bool outgoing,
                          bool contact, uint32_t packetId) {
    if (!protocolValid(protocol) || !peerId || strlen(peerId) >= 70 || length > 240 || (length && !payload) || count_ == QUEUE || lastId_ == UINT32_MAX) {
        fail("Message persistence queue unavailable"); return 0;
    }
    ChatRecord record{}; record.id = ++lastId_; record.timestamp = timestamp; record.port = port;
    record.length = length; record.packetId = packetId; record.flags = direct | (outgoing << 1) | (contact << 2);
    snprintf(record.protocol, sizeof(record.protocol), "%s", protocol);
    snprintf(record.peerId, sizeof(record.peerId), "%s", peerId);
    snprintf(record.peer, sizeof(record.peer), "%s", peer ? peer : "");
    if (length) memcpy(record.payload, payload, length);
    queue_[(head_ + count_++) % QUEUE] = record; ++revision_; return record.id;
}
bool ChatStore::erase(uint32_t id) {
    bool found = false;
    for (size_t i = 0; i < indexCount_; ++i) found |= index_[i].id == id;
    for (unsigned i = 0; i < count_; ++i) {
        auto &record = queue_[(head_ + i) % QUEUE];
        if (record.kind == 1 && record.id == id) { record.kind = 2; record.targetId = id; record.length = 0; ++revision_; return true; }
    }
    if (!found || count_ == QUEUE || lastId_ == UINT32_MAX) { fail("Cannot persist deletion"); return false; }
    ChatRecord record{}; record.kind = 2; record.id = ++lastId_; record.targetId = id;
    queue_[(head_ + count_++) % QUEUE] = record; ++revision_; return true;
}
bool ChatStore::clearHistory() {
    if (!initialized_) { fail("Chat storage unavailable"); return false; }
    // An explicit reset also prevents old snapshots from being imported on boot.
    char path[192]; snprintf(path, sizeof(path), "%s/legacy-import.done", root_);
    const uint32_t magic = 0x31474c57;
    FILE *marker = fopen(path, "wb");
    bool marked = marker && fwrite(&magic, 1, sizeof(magic), marker) == sizeof(magic);
    if (marker) marked = durableClose(marker) && marked;
    if (!marked) { fail("Cannot mark history reset"); return false; }
    snprintf(path, sizeof(path), "%s/messages", root_);
    DIR *dir = opendir(path);
    if (!dir) { fail("Cannot scan message directory"); return false; }
    bool ok = true;
    while (auto *entry = readdir(dir)) {
        unsigned number = 0; char extra = 0;
        if (strlen(entry->d_name) != 12 || strspn(entry->d_name, "0123456789") != 8 ||
            sscanf(entry->d_name, "%8u.log%c", &number, &extra) != 1 || !number) continue;
        segmentPath(number, path, sizeof(path));
        if (unlink(path) != 0) { ok = false; break; }
    }
    closedir(dir);
    if (!ok) { fail("Cannot remove message history"); return false; }
    indexCount_ = segmentCount_ = head_ = count_ = 0;
    current_ = 1; writeOffset_ = -1; retryAfter_ = 0; error_[0] = 0;
    ++revision_;
    return true;
}
bool ChatStore::read(const Index &entry, ChatRecord &record) const {
    char path[192]; segmentPath(entry.segment, path, sizeof(path)); FILE *file = fopen(path, "rb");
    if (!file) return false;
    const bool ok = fseek(file, size_t(entry.slot) * sizeof(record), SEEK_SET) == 0 && fread(&record, 1, sizeof(record), file) == sizeof(record) && valid(record);
    fclose(file); return ok;
}
size_t ChatStore::history(ChatRecord *out, size_t limit, const char *protocol, const char *peerId, int direct, uint32_t before, bool textOnly) {
    if (!out || !limit) return 0;
    limit = limit > 50 ? 50 : limit; size_t used = 0;
    auto deleted = [&](uint32_t id) { for (unsigned i = 0; i < count_; ++i) { const auto &r = queue_[(head_ + i) % QUEUE]; if (r.kind == 2 && r.targetId == id) return true; } return false; };
    auto match = [&](const ChatRecord &r) { return r.kind == 1 && (!before || r.id < before) && !deleted(r.id) &&
        (!protocol || !strcmp(protocol, r.protocol)) && (!peerId || !strcmp(peerId, r.peerId)) &&
        (direct < 0 || bool(r.flags & 1) == bool(direct)) && (!textOnly || r.port == 1); };
    for (unsigned i = count_; i && used < limit; --i) { const auto &r = queue_[(head_ + i - 1) % QUEUE]; if (match(r)) out[used++] = r; }
    for (size_t i = indexCount_; i && used < limit; --i) {
        const auto &entry = index_[i - 1];
        if ((before && entry.id >= before) || (protocol && entry.protocolHash != hash(protocol)) ||
            (peerId && entry.peerHash != hash(peerId)) || (direct >= 0 && bool(entry.flags & 1) != bool(direct)) ||
            (textOnly && entry.port != 1) || deleted(entry.id)) continue;
        ChatRecord record{}; if (read(entry, record) && match(record)) out[used++] = record;
    }
    return used;
}
bool ChatStore::flushOne() {
    if (!count_) return true;
    char path[192]; segmentPath(current_, path, sizeof(path)); struct stat info{};
    size_t size = stat(path, &info) == 0 ? size_t(info.st_size) : 0;
    if (writeOffset_ >= 0) size = size_t(writeOffset_);
    if (size >= SLOTS * sizeof(ChatRecord)) {
        if (current_ >= 99999999) { fail("Segment counter exhausted"); return false; }
        ++current_; size = 0; segmentPath(current_, path, sizeof(path));
    }
    FILE *file = fopen(path, "r+b");
    if (!file && errno == ENOENT) file = fopen(path, "w+b");
    if (!file) { fail("Cannot append message segment"); return false; }
    // Retry the same slot after a failed flush, rather than appending duplicate IDs.
    if (fseek(file, size, SEEK_SET) != 0) { fclose(file); fail("Cannot seek message segment"); return false; }
    if (size % sizeof(ChatRecord)) {
        const uint8_t padding[512]{}; const size_t gap = 512 - size % 512;
        if (fwrite(padding, 1, gap, file) != gap) { fclose(file); fail("Cannot recover message tail"); return false; }
        size += gap;
        if (size == SLOTS * sizeof(ChatRecord)) { durableClose(file); ++current_; return flushOne(); }
    }
    auto record = queue_[head_]; record.crc = crc32(reinterpret_cast<const uint8_t *>(&record) + 8, 504);
    writeOffset_ = long(size);
    errno = 0;
    const bool written = fwrite(&record, 1, sizeof(record), file) == sizeof(record);
    int error = written ? 0 : errno; const char *stage = "write";
    if (fflush(file) != 0 && !error) { error = errno ? errno : EIO; stage = "flush"; }
    if (fsync(fileno(file)) != 0 && !error) { error = errno ? errno : EIO; stage = "sync"; }
    if (fclose(file) != 0 && !error) { error = errno ? errno : EIO; stage = "close"; }
    if (error || !written) {
        snprintf(error_, sizeof(error_), "Message %s failed (%d)", stage, error ? error : EIO);
#ifdef ARDUINO
        Serial.printf("Chats: %s path=%s offset=%u\n", error_, path, unsigned(size));
#endif
        return false;
    }
    writeOffset_ = -1;
    if (!segmentCount_ || segments_[segmentCount_ - 1] != current_) {
        if (segmentCount_ == SEGMENTS) {
            const uint32_t removed = segments_[0]; segmentPath(removed, path, sizeof(path));
            if (unlink(path)) { fail("Cannot rotate history"); return false; }
            memmove(segments_, segments_ + 1, (--segmentCount_) * sizeof(uint32_t));
            for (size_t i = 0; i < indexCount_;) {
                if (index_[i].segment == removed) memmove(index_ + i, index_ + i + 1, (--indexCount_ - i) * sizeof(Index));
                else ++i;
            }
        }
        segments_[segmentCount_++] = current_;
    }
    apply(record, current_, size / sizeof(record)); head_ = (head_ + 1) % QUEUE; --count_; error_[0] = 0; return true;
}
const StoredPeer *ChatStore::peer(size_t index) const { return index < peerCount_ ? peers_ + index : nullptr; }
bool ChatStore::upsertPeer(const StoredPeer &peer) {
    if (!peers_ || !protocolValid(peer.protocol) || !peer.peer.id[0]) return false;
    size_t index = 0; while (index < peerCount_ && (strcmp(peers_[index].protocol, peer.protocol) || strcmp(peers_[index].peer.id, peer.peer.id))) ++index;
    if (index == PEERS) { fail("Contact store full"); return false; }
    if (index < peerCount_ && !strcmp(peers_[index].peer.name, peer.peer.name) && peers_[index].peer.contact == peer.peer.contact &&
        !memcmp(peers_[index].publicKey, peer.publicKey, 32)) return true;
    peers_[index] = peer; if (index == peerCount_) ++peerCount_; contactsDirty_ = true; return true;
}
bool ChatStore::forgetPeer(const char *protocol, const char *id) {
    for (size_t i = 0; i < peerCount_; ++i) if (!strcmp(peers_[i].protocol, protocol) && !strcmp(peers_[i].peer.id, id)) {
        memmove(peers_ + i, peers_ + i + 1, (--peerCount_ - i) * sizeof(StoredPeer)); contactsDirty_ = true; return true;
    }
    return false;
}
bool ChatStore::savePeers() {
    JsonDocument doc(&jsonAllocator); doc["version"] = 1; doc["generation"] = generation_ + 1;
    auto entries = doc["contacts"].to<JsonArray>();
    for (size_t i = 0; i < peerCount_; ++i) {
        const auto &peer = peers_[i]; auto item = entries.add<JsonObject>();
        item["protocol"] = peer.protocol; item["id"] = peer.peer.id; item["name"] = peer.peer.name; item["contact"] = peer.peer.contact;
        char key[65]{}; for (unsigned j = 0; j < 32; ++j) snprintf(key + 2 * j, 3, "%02x", peer.publicKey[j]); item["publicKey"] = key;
    }
    doc["crc32"] = peerChecksum(peers_, peerCount_);
    if (doc.overflowed()) { fail("Contact JSON allocation failed"); return false; }
    char tmp[192], current[192], backup[192]; snprintf(tmp, sizeof(tmp), "%s/contacts.tmp", root_);
    snprintf(current, sizeof(current), "%s/contacts.json", root_); snprintf(backup, sizeof(backup), "%s/contacts.backup.json", root_);
    const size_t size = measureJson(doc); char *data = static_cast<char *>(allocate(size + 1));
    if (!data) { fail("Contact buffer allocation failed"); return false; }
    serializeJson(doc, data, size + 1); FILE *file = fopen(tmp, "wb");
    bool written = file && fwrite(data, 1, size, file) == size; free(data);
    if (file) written = durableClose(file) && written;
    if (!written) { fail("Contact write failed"); return false; }
    // Keep one verified snapshot through replacement; recover from either on boot.
    if (loadPeers(current, false)) {
        unlink(backup); if (rename(current, backup)) { fail("Contact backup failed"); return false; }
    } else unlink(current);
    if (rename(tmp, current)) { fail("Contact replacement failed"); return false; }
    ++generation_; contactsDirty_ = false; error_[0] = 0; return true;
}
bool ChatStore::loadPeers(const char *path, bool applySnapshot) {
    FILE *file = fopen(path, "rb"); if (!file) return false;
    fseek(file, 0, SEEK_END); const long size = ftell(file); rewind(file);
    if (size <= 0 || size > 32768) { fclose(file); return false; }
    char *data = static_cast<char *>(allocate(size)); if (!data) { fclose(file); return false; }
    const bool readOk = fread(data, 1, size, file) == size; fclose(file);
    JsonDocument doc(&jsonAllocator); const auto error = deserializeJson(doc, data, size); free(data);
    if (!readOk || error || doc["version"].as<unsigned>() != 1 || !doc["generation"].is<uint32_t>() || !doc["crc32"].is<uint32_t>()) return false;
    const auto entries = doc["contacts"].as<JsonArray>(); if (entries.isNull() || entries.size() > PEERS) return false;
    auto *snapshot = static_cast<StoredPeer *>(allocate(sizeof(StoredPeer) * PEERS)); if (!snapshot) return false;
    size_t count = 0; bool valid = true;
    for (auto item : entries) {
        auto &peer = snapshot[count]; peer = {};
        const char *protocol = item["protocol"] | "", *id = item["id"] | "", *name = item["name"] | "", *key = item["publicKey"] | "";
        if (!protocolValid(protocol) || !*id || strlen(id) >= sizeof(peer.peer.id) || strlen(name) >= sizeof(peer.peer.name) || strlen(key) != 64 || !item["contact"].is<bool>()) { valid = false; break; }
        snprintf(peer.protocol, sizeof(peer.protocol), "%s", protocol); snprintf(peer.peer.id, sizeof(peer.peer.id), "%s", id);
        snprintf(peer.peer.name, sizeof(peer.peer.name), "%s", name); peer.peer.contact = item["contact"];
        for (unsigned i = 0; i < 32; ++i) { unsigned byte = 0; char pair[3] = {key[i * 2], key[i * 2 + 1], 0};
            if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%x", &byte) != 1) { valid = false; break; } peer.publicKey[i] = byte;
        }
        for (size_t i = 0; i < count; ++i) if (!strcmp(snapshot[i].protocol, protocol) && !strcmp(snapshot[i].peer.id, id)) valid = false;
        ++count;
    }
    valid = valid && doc["crc32"].as<uint32_t>() == peerChecksum(snapshot, count);
    if (valid && applySnapshot && doc["generation"].as<uint32_t>() >= generation_) {
        generation_ = doc["generation"]; peerCount_ = count; memcpy(peers_, snapshot, count * sizeof(StoredPeer));
    }
    free(snapshot); return valid;
}
void ChatStore::poll(bool available, uint32_t now) {
    if (!available) { if (count_ || contactsDirty_) fail("SD unavailable; changes pending in RAM"); return; }
    if (now && retryAfter_ && int32_t(now - retryAfter_) < 0) return;
    if (!initialized_ && !begin()) { retryAfter_ = now + 5000; return; }
    const bool ok = count_ ? flushOne() : !contactsDirty_ || savePeers();
    retryAfter_ = ok ? 0 : now + 5000;
}

bool ChatStore::importLegacy(const char *path) {
    if (!initialized_) return false;
    char marker[192]; snprintf(marker, sizeof(marker), "%s/legacy-import.done", root_);
    const uint32_t magic = 0x31474c57; uint32_t saved = 0;
    FILE *file = fopen(marker, "rb");
    if (file) { const bool complete = fread(&saved, 1, 4, file) == 4 && saved == magic; fclose(file); if (complete) return true; }
    file = fopen(path, "rb"); if (!file) return true;
    fseek(file, 0, SEEK_END); const long size = ftell(file); rewind(file);
    if (size <= 0 || size > 32768) { fclose(file); fail("Invalid legacy chat snapshot"); return false; }
    char *data = static_cast<char *>(allocate(size)); if (!data) { fclose(file); return false; }
    const bool readOk = fread(data, 1, size, file) == size; fclose(file);
    JsonDocument doc(&jsonAllocator); const auto result = deserializeJson(doc, data, size); free(data);
    const char *protocol = doc["protocol"] | ""; const auto messages = doc["messages"].as<JsonArray>();
    if (!readOk || result || !protocolValid(protocol) || messages.isNull() || messages.size() > 24) { fail("Invalid legacy chat snapshot"); return false; }
    auto *existing = static_cast<ChatRecord *>(allocate(24 * sizeof(ChatRecord))); if (!existing) return false;
    const size_t count = history(existing, 24, protocol, nullptr, -1, 0, true);
    for (auto message : messages) {
        const char *text = message["text"] | "", *name = message["peer"] | ""; const size_t length = strlen(text);
        if (length > 240 || strlen(name) > 39 || !message["timestamp"].is<uint32_t>() || !message["direct"].is<bool>() || !message["outgoing"].is<bool>()) {
            free(existing); fail("Invalid legacy message"); return false;
        }
        const uint32_t timestamp = message["timestamp"]; const bool direct = message["direct"], outgoing = message["outgoing"];
        bool found = false;
        for (size_t i = 0; i < count; ++i) if (existing[i].timestamp == timestamp && bool(existing[i].flags & 1) == direct &&
            bool(existing[i].flags & 2) == outgoing && !strcmp(existing[i].peer, name) && existing[i].length == length && !memcmp(existing[i].payload, text, length)) found = true;
        if (found) continue; const char *id = "";
        for (size_t i = 0; i < peerCount_; ++i) if (!strcmp(peers_[i].protocol, protocol) && !strcmp(peers_[i].peer.name, name)) { id = peers_[i].peer.id; break; }
        if (!append(protocol, id, name, timestamp, 1, reinterpret_cast<const uint8_t *>(text), length, direct, outgoing, message["contact"] | false) || !flushOne()) {
            free(existing); return false;
        }
    }
    free(existing); file = fopen(marker, "wb");
    bool ok = file && fwrite(&magic, 1, 4, file) == 4; if (file) ok = durableClose(file) && ok;
    if (!ok) fail("Cannot mark legacy migration complete"); return ok;
}
