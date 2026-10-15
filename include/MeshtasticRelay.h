#pragma once
#include <meshtastic_packet.h>
#include <cstring>
// Classic managed flooding: keep ciphertext and identity intact, change only
// the remaining hop count and last relayer. No payload decryption is required.
inline size_t buildClientRelay(const uint8_t *frame, size_t length, uint32_t local,
                              bool enabled, uint8_t *output, size_t capacity) {
    MeshRxPacket packet{};
    if (!enabled || length > capacity || !meshParsePacket(frame, length, &packet) ||
        !packet.from || packet.from == MESH_ADDR_BROADCAST || packet.from == local ||
        packet.to == local || !packet.hop_limit || packet.via_mqtt ||
        (packet.next_hop && packet.next_hop != uint8_t(local))) return 0;
    memcpy(output, frame, length);
    output[12] = (output[12] & ~MESH_FLAGS_HOP_LIMIT_MASK) | (packet.hop_limit - 1);
    output[14] = 0; // Flood fallback; this backend does not maintain next-hop routes.
    output[15] = uint8_t(local);
    return length;
}
