#pragma once
#include <meshtastic.h>
#include <math.h>

// Endpoint response: retain intermediate hops and add only this hop's SNR.
// RouteDiscovery repeated fields accept both packed and unpacked protobuf forms.
inline size_t buildTraceRoutePayload(const uint8_t *input, size_t length, float snr,
                                    uint8_t *output, size_t capacity) {
    if (length > 200 || capacity < length + 11 || !isfinite(snr)) return 0;
    unsigned counts[4]{}; auto cursor = pbCursor(input, length);
    while (!cursor.exhausted()) {
        uint32_t field; uint8_t wire;
        if (!cursor.readTag(&field, &wire) || !field) return 0;
        if (field > 4) { if (!cursor.skipField(wire)) return 0; continue; }
        const bool fixed = field == 1 || field == 3;
        auto readOne = [&](PbCursor &c) {
            uint32_t value; uint64_t v;
            if (!(fixed ? c.readFixed32(&value) : c.readVarint(&v))) return false;
            return ++counts[field - 1] <= 8;
        };
        if (wire == (fixed ? 5 : 0)) { if (!readOne(cursor)) return 0; }
        else if (wire == 2) {
            uint64_t size;
            if (!cursor.readVarint(&size) || size > cursor.remaining()) return 0;
            auto packed = pbCursor(cursor.buf + cursor.pos, size);
            while (!packed.exhausted()) if (!readOne(packed)) return 0;
            cursor.pos += size;
        } else return 0;
    }
    if (counts[1] >= 8) return 0;
    if (length) memcpy(output, input, length);
    auto writer = pbWriter(output + length, capacity - length);
    const int32_t q4 = int32_t(lroundf(fmaxf(-127, fminf(127, snr * 4))));
    if (!writer.writeVarintField(2, uint64_t(int64_t(q4)))) return 0;
    return length + writer.written();
}
inline size_t buildResponseData(MeshPortNum port, const uint8_t *payload, size_t length,
                               uint32_t from, uint32_t to, uint32_t request,
                               uint8_t *output, size_t capacity) {
    auto writer = pbWriter(output, capacity);
    if (!writer.writeVarintField(1, port) || !writer.writeBytes(2, payload, length) ||
        !writer.writeFixed32Field(4, to) || !writer.writeFixed32Field(5, from) ||
        !writer.writeFixed32Field(6, request)) return 0;
    return writer.written();
}
