#include "ChatStore.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>
int main() {
    char temp[] = "/tmp/wio-chat-store-XXXXXX"; assert(mkdtemp(temp));
    const std::string root = std::string(temp) + "/appdata/chats";
    uint32_t first = 0, second = 0;
    {
        ChatStore store(root.c_str()); assert(store.begin());
        first = store.append("meshtastic", "!11223344", "武汉", 123, 1, (const uint8_t *)"hello", 5, true, false, true, 0xaabbccdd);
        const uint8_t binary[] = {0,0xff,3};
        second = store.append("meshtastic", "!11223344", "武汉", 124, 67, binary, sizeof(binary), true, false, false);
        ChatRecord records[3]; assert(store.history(records, 3) == 2 && records[0].id == second);
        while (store.pending()) store.poll(true);
        StoredPeer peer{}; strcpy(peer.protocol, "meshtastic"); strcpy(peer.peer.id, "!11223344"); strcpy(peer.peer.name, "武汉"); peer.peer.contact = true; peer.publicKey[0] = 8;
        assert(store.upsertPeer(peer)); store.poll(true);
        strcpy(peer.peer.name, "Renamed"); assert(store.upsertPeer(peer)); store.poll(true);
    }
    {
        ChatStore store(root.c_str()); assert(store.begin());
        assert(store.peerCount() == 1 && !strcmp(store.peer(0)->peer.name, "Renamed"));
        ChatRecord records[3]; assert(store.history(records, 3, "meshtastic", "!11223344", 1, 0, true) == 1);
        assert(records[0].id == first && records[0].packetId == 0xaabbccdd && records[0].length == 5);
        assert(store.history(records, 3, "meshtastic", "!11223344", 1, second) == 1);
        assert(store.erase(first)); assert(store.history(records, 3) == 1); store.poll(true);
    }
    // A torn tail does not erase earlier records or prevent subsequent appends.
    const std::string segment = root + "/messages/00000001.log";
    FILE *file = fopen(segment.c_str(), "ab"); assert(file); fwrite("torn", 1, 4, file); fclose(file);
    {
        ChatStore store(root.c_str()); assert(store.begin()); assert(store.messageCount() == 1);
        assert(store.append("meshcore", "", "", 125, 1, (const uint8_t *)"tail recovered", 14, false, true, false)); store.poll(true);
    }
    // Corrupted current contacts recover from the verified backup.
    file = fopen((root + "/contacts.json").c_str(), "wb"); assert(file); fwrite("bad", 1, 3, file); fclose(file);
    {
        ChatStore store(root.c_str()); assert(store.begin()); assert(store.messageCount() == 2);
        assert(store.peerCount() == 1 && !strcmp(store.peer(0)->peer.name, "武汉"));
        for (unsigned i = 0; i < 2200; ++i) {
            assert(store.append("meshtastic", "!11223344", "Renamed", i, 1, (const uint8_t *)"rotation", 8, true, false, false)); store.poll(true);
        }
        assert(store.messageCount() <= 2048 && store.messageCount() > 1900);
    }
    {
        ChatStore store(root.c_str()); assert(store.begin()); ChatRecord records[50];
        assert(store.messageCount() <= 2048 && store.messageCount() > 1900);
        assert(store.history(records, 50, "meshtastic", "!11223344", 1) == 50);
        const uint32_t cursor = records[49].id;
        assert(store.history(records, 50, "meshtastic", "!11223344", 1, cursor) == 50 && records[0].id < cursor);
        for (unsigned i = 0; i < 8; ++i) assert(store.append("meshtastic", "", "", i, 1, nullptr, 0, false, false, false));
        assert(!store.append("meshtastic", "", "", 0, 1, nullptr, 0, false, false, false));
        store.poll(false); assert(strstr(store.error(), "SD unavailable"));
        while (store.pending()) store.poll(true);
    }
    const std::string legacyRoot = std::string(temp) + "/legacy/chats";
    {
        ChatStore store(root.c_str()); assert(store.begin());
        const auto peers = store.peerCount(); assert(peers);
        assert(store.append("meshtastic", "", "", 0, 1, nullptr, 0, false, false, false));
        assert(store.clearHistory()); assert(!store.pending() && !store.messageCount());
        assert(store.peerCount() == peers);
        assert(store.append("meshtastic", "", "", 1, 1, (const uint8_t *)"fresh", 5, false, false, false));
        store.poll(true); assert(!store.pending());
    }
    {
        ChatStore store(root.c_str()); assert(store.begin());
        assert(store.messageCount() == 1 && store.peerCount());
        ChatRecord record; assert(store.history(&record, 1) == 1 && record.length == 5);
        assert(store.erase(record.id)); store.poll(true); assert(!store.messageCount());
    }
    { ChatStore store(root.c_str()); assert(store.begin()); assert(!store.messageCount()); }
    const std::string legacyFile = std::string(temp) + "/legacy.json";
    file = fopen(legacyFile.c_str(), "wb"); assert(file);
    const char *snapshot = R"({"protocol":"meshtastic","messages":[{"text":"imported","peer":"Alice","timestamp":100,"direct":false,"outgoing":false}]})";
    fwrite(snapshot, 1, strlen(snapshot), file); fclose(file);
    {
        ChatStore store(legacyRoot.c_str()); assert(store.begin());
        assert(store.importLegacy(legacyFile.c_str())); assert(store.messageCount() == 1);
        assert(store.importLegacy(legacyFile.c_str())); assert(store.messageCount() == 1);
    }
    {
        ChatStore store(legacyRoot.c_str()); assert(store.begin());
        assert(store.importLegacy(legacyFile.c_str())); assert(store.messageCount() == 1);
        assert(store.clearHistory());
    }
    {
        ChatStore store(legacyRoot.c_str()); assert(store.begin());
        assert(store.importLegacy(legacyFile.c_str())); assert(store.messageCount() == 0);
    }
    std::filesystem::remove_all(temp);
    puts("ChatStore reboot, stable identity, binary data, durable delete, torn tail, contact backup, rotation and pagination passed");
}
