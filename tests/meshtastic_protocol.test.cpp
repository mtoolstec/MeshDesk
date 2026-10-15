#include "meshtastic_crypto_openssl.h"
#include <meshtastic.h>
#include "MeshtasticResponses.h"
#include "MeshtasticRelay.h"
#include <cstdio>
static uint32_t testRandom() { return 0x12345678; }
int main() {
    MeshSession alice{}, bob{};
    alice.init(REGION_US, MODEM_LONG_FAST, ROLE_CLIENT_MUTE, 0x25c21b88);
    bob.init(REGION_US, MODEM_LONG_FAST, ROLE_CLIENT_MUTE, 0x11223344);
    alice.channels.addDefaultChannel(); bob.channels.addDefaultChannel();
    assert(alice.radioConfig().frequency_mhz == 906.875f);
    assert(alice.channels.channels[0].hash == 8);
    const uint8_t encrypted[] = {0x2d,0x73,0xfe,0xa3,0x70,0x6e,0xbf,0x6a,0x16,0xe0};
    uint8_t frame[256]{};
    size_t n = meshBuildPacket(frame, MESH_ADDR_BROADCAST, alice.node_num, 0xaba472a5, 3, 3, false, false, 8, 0, 0, encrypted, sizeof(encrypted));
    MeshRxResult result{};
    assert(bob.processRx(frame, n, -60, 7, &result));
    assert(result.data.portnum == PORT_TEXT_MESSAGE && result.data.payload_len == 4 && !memcmp(result.data.payload, "Test", 4));
    const uint8_t truncated[] = {0x08,1,0x80}; MeshData data{};
    assert(!meshDecodeData(truncated, sizeof(truncated), &data));
    const uint8_t wrongWire[] = {0x0a,1,1}; assert(!meshDecodeData(wrongWire, sizeof(wrongWire), &data));
    MeshRxPacket packet{}; assert(!meshParsePacket(frame, 256, &packet));
    assert(alice.pki.generate() && bob.pki.generate());
    alice.node_keys.setKey(bob.node_num, bob.pki.public_key); bob.node_keys.setKey(alice.node_num, alice.pki.public_key);
    n = alice.buildTextDm(bob.node_num, "PKI Test", true, frame, testRandom);
    assert(n && bob.processRx(frame, n, -70, 6, &result));
    assert(result.is_pki && result.data.payload_len == 8 && !memcmp(result.data.payload, "PKI Test", 8));
    frame[20] ^= 1; assert(!bob.processRx(frame, n, -70, 6, &result));
    // The packet maximum includes the header and PKI overhead.
    char huge[235]; memset(huge, 'A', sizeof(huge) - 1); huge[sizeof(huge) - 1] = 0;
    assert(!alice.buildTextDm(bob.node_num, huge, true, frame, testRandom));
    uint8_t payload[240]; memset(payload, 1, sizeof(payload));
    assert(!alice.buildTx(0, MESH_ADDR_BROADCAST, PORT_TEXT_MESSAGE, payload, sizeof(payload), false, false, frame));
    // A long legal packet still stays inside the 255-byte radio frame.
    char legal[221]; memset(legal, 'B', 220); legal[220] = 0; frame[255] = 0xa5;
    n = alice.buildTextDm(bob.node_num, legal, true, frame, testRandom);
    assert(n <= 255 && n > 0 && frame[255] == 0xa5 && bob.processRx(frame, n, -65, 8, &result));
    // Unknown fields remain forward compatible; invalid encodings fail closed.
    const uint8_t future[] = {0x08,1,0x12,1,'x',0x58,1}; assert(meshDecodeData(future, sizeof(future), &data));
    // Empty direct trace, relayed trace, negative SNR, malformed routes and response correlation.
    uint8_t route[220], response[239];
    size_t routeLen = buildTraceRoutePayload(nullptr, 0, -6.5f, route, sizeof(route));
    assert(routeLen == 11); auto cursor = pbCursor(route, routeLen);
    uint32_t field; uint8_t wire; uint64_t value;
    assert(cursor.readTag(&field, &wire) && field == 2 && wire == 0);
    assert(cursor.readVarint(&value) && int32_t(value) == -26 && cursor.exhausted());
    const uint8_t relayed[] = {0x0a,4,0x78,0x56,0x34,0x12,0x12,1,24};
    routeLen = buildTraceRoutePayload(relayed, sizeof(relayed), 7, route, sizeof(route));
    assert(routeLen == sizeof(relayed) + 2 && !memcmp(route, relayed, sizeof(relayed)));
    const uint8_t broken[] = {0x0a,3,1,2,3};
    assert(!buildTraceRoutePayload(broken, sizeof(broken), 7, route, sizeof(route)));
    const uint8_t overflow[] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,2};
    cursor = pbCursor(overflow, sizeof(overflow)); assert(!cursor.readVarint(&value));
    n = buildResponseData(PORT_TRACEROUTE, route, routeLen, bob.node_num, alice.node_num, 0x12345678, response, sizeof(response));
    assert(n && meshDecodeData(response, n, &data));
    assert(data.portnum == PORT_TRACEROUTE && data.request_id == 0x12345678 && data.source == bob.node_num && data.dest == alice.node_num && !data.want_response);
    assert(!buildResponseData(PORT_TRACEROUTE, route, routeLen, bob.node_num, alice.node_num, 1, response, 4));
    const uint32_t responseId = 0x456789ab;
    assert(meshEncrypt(&bob.channels.channels[0].key, bob.node_num, responseId, response, n));
    size_t replyLen = meshBuildPacket(frame, alice.node_num, bob.node_num, responseId, 3, 3, false, false, 8, 0, uint8_t(bob.node_num), response, n);
    assert(alice.processRx(frame, replyLen, -60, 5, &result));
    assert(result.data.request_id == 0x12345678 && result.data.portnum == PORT_TRACEROUTE);

    // Relay only eligible packets, preserving encrypted bytes and packet identity.
    uint8_t relayedFrame[255];
    n = alice.buildTextTx(0, MESH_ADDR_BROADCAST, "relay test", false, false, frame);
    size_t relaySize = buildClientRelay(frame, n, bob.node_num, true, relayedFrame, sizeof(relayedFrame));
    assert(relaySize == n && meshParsePacket(relayedFrame, relaySize, &packet));
    assert(packet.hop_limit == 2 && packet.hop_start == 3 && packet.from == alice.node_num);
    assert(packet.relay_node == uint8_t(bob.node_num) && !memcmp(frame + 16, relayedFrame + 16, n - 16));
    assert(!buildClientRelay(frame, n, bob.node_num, false, relayedFrame, sizeof(relayedFrame)));
    assert(!buildClientRelay(frame, n, alice.node_num, true, relayedFrame, sizeof(relayedFrame)));
    frame[12] &= ~MESH_FLAGS_HOP_LIMIT_MASK;
    assert(!buildClientRelay(frame, n, bob.node_num, true, relayedFrame, sizeof(relayedFrame)));
    n = alice.buildTextDm(bob.node_num, "opaque private", true, frame, testRandom);
    assert(!buildClientRelay(frame, n, bob.node_num, true, relayedFrame, sizeof(relayedFrame)));
    assert(buildClientRelay(frame, n, 0x87654321, true, relayedFrame, sizeof(relayedFrame)) == n);
    frame[14] = 0x55;
    assert(!buildClientRelay(frame, n, 0x87654321, true, relayedFrame, sizeof(relayedFrame)));
    frame[14] = 0; frame[12] |= MESH_FLAGS_VIA_MQTT_MASK;
    assert(!buildClientRelay(frame, n, 0x87654321, true, relayedFrame, sizeof(relayedFrame)));

    const uint8_t compactRequest[] = {8,70,0x48,2};
    assert(meshDecodeData(compactRequest, sizeof(compactRequest), &data) && data.want_response);
    puts("Meshtastic golden AES frame, PKI authentication, frame bounds and parsing passed");
}
