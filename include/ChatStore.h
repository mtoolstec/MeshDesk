#pragma once
#include <cstddef>
#include <cstdint>
#include "CommunicationService.h"

// Versioned fixed-size little-endian records. CRC covers bytes 8..511.
#pragma pack(push, 1)
struct ChatRecord {
    uint32_t magic = 0x31434857, crc = 0, id = 0, timestamp = 0, packetId = 0, targetId = 0;
    uint16_t port = 1, length = 0;
    uint8_t kind = 1, flags = 0; // 1: data, 2: deletion; direct/outgoing/contact bits
    char protocol[12]{}, peerId[70]{}, peer[40]{};
    uint8_t payload[240]{}, reserved[120]{};
};
#pragma pack(pop)
static_assert(sizeof(ChatRecord) == 512, "Chat log format must be stable");
struct StoredPeer {
    char protocol[12]{};
    CommunicationPeer peer{};
    uint8_t publicKey[32]{};
};
class ChatStore {
public:
    explicit ChatStore(const char *root = "/sdcard/appdata/chats");
    ~ChatStore();
    ChatStore(const ChatStore &) = delete;
    ChatStore &operator=(const ChatStore &) = delete;
    bool begin();
    bool importLegacy(const char *path);
    void poll(bool available, uint32_t now = 0);
    uint32_t append(const char *protocol, const char *peerId, const char *peer, uint32_t timestamp,
                    uint16_t port, const uint8_t *payload, size_t length, bool direct, bool outgoing,
                    bool contact, uint32_t packetId = 0);
    bool erase(uint32_t id);
    bool clearHistory();
    // Newest first; before=0 starts from latest. Hashes only accelerate matching:
    // full identities are verified when reading records.
    size_t history(ChatRecord *out, size_t limit, const char *protocol = nullptr,
                   const char *peerId = nullptr, int direct = -1, uint32_t before = 0, bool textOnly = false);
    bool upsertPeer(const StoredPeer &peer);
    bool forgetPeer(const char *protocol, const char *id);
    size_t peerCount() const { return peerCount_; }
    const StoredPeer *peer(size_t index) const;
    uint32_t revision() const { return revision_; }
    unsigned pending() const { return count_; }
    size_t messageCount() const { return indexCount_; }
    bool ready() const { return initialized_; }
    const char *error() const { return error_; }
private:
    struct Index { uint32_t id, segment, protocolHash, peerHash; uint16_t slot, port; uint8_t flags; };
    static constexpr unsigned SEGMENTS = 16, SLOTS = 128, QUEUE = 8, PEERS = 64;
    char root_[144]{}, error_[96]{};
    Index *index_ = nullptr;
    StoredPeer *peers_ = nullptr;
    size_t indexCount_ = 0, peerCount_ = 0;
    uint32_t retryAfter_ = 0;
    uint32_t segments_[SEGMENTS]{}, current_ = 0, lastId_ = 0, generation_ = 0, revision_ = 0;
    unsigned segmentCount_ = 0, head_ = 0, count_ = 0;
    long writeOffset_ = -1;
    ChatRecord queue_[QUEUE]{};
    bool initialized_ = false, contactsDirty_ = false;
    void fail(const char *message);
    void apply(const ChatRecord &record, uint32_t segment, unsigned slot);
    bool read(const Index &entry, ChatRecord &record) const;
    bool flushOne();
    bool savePeers();
    bool loadPeers(const char *file, bool apply);
    void segmentPath(uint32_t segment, char *path, size_t size) const;
};
extern ChatStore chatStore;
