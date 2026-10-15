# Communication backends

`CommunicationService` owns the active backend: `off`, `meshcore`, or `meshtastic`.
The two implementations share one SX1262 instance through `WioRadio` and stop
before handing it over. Protocol choice and each family's radio configuration
are stored independently. A failed start attempts to restore the previous
backend. System time is independent of radio state.

Native Radio selects protocol, modem preset and (for Meshtastic) region. Native
chat and PocketJS use portable peers and messages. Messages retain their source
protocol and are filtered when switching protocols. Meshtastic node names, contact flags, received public keys, identity and
configuration persist across reboots. Live last-seen times remain in RAM. Contacts and locally decoded text/data
history persist under `/appdata/chats` (see [storage](CHAT_STORAGE.md)). Missing sender keys trigger a directed NodeInfo request; up to
two encrypted messages wait for key exchange for 45 seconds, then expire.

## Meshtastic client

The implementation uses the pinned BSD-3-Clause MeshtasticLite protocol library,
not the GPL Meshtastic firmware. It implements classic channel AES-CTR packets,
NodeInfo/public-key exchange, X25519/SHA256/AES-CCM direct messages, routing ACKs,
endpoint Traceroute responses (request correlation and receive SNR), frame
deduplication, and a bounded asynchronous RF queue. Retransmitted requests can
receive another reply when the original response was lost. Radio and JS support persistent CLIENT / CLIENT_MUTE selection. CLIENT_MUTE
sends/receives its own messages without forwarding. CLIENT relays eligible
classic channel packets and opaque PKI packets with remaining hops, using two
pending relay slots, SNR-weighted delay, duplicate cancellation and hop decrement.
It uses flooding fallback rather than a next-hop route table; it does not enrich
intermediate Traceroute paths or implement the full official routing stack. Radio uses sync word 0x2b, preamble 16,
explicit headers and CRC. Direct messages require a learned recipient public key.

This is a limited client, not a complete Meshtastic router or official firmware.
No BLE administration, secondary channels, automatic lost-message retries,
channel AEAD or signed broadcasts are implemented. Key exchange follows the
library's NodeInfo behavior; there is no separate trusted-key verification UI.
Peers requiring signed broadcasts or channel AEAD are not compatible with the
classic channel implementation. A shared-channel ACK is not proof of an
individually authenticated recipient.

## Current validation

- Native facade tests: selection, off, failure rollback and saved configuration.
- Protocol tests: independent AES golden frame, PKI round trip and tamper rejection,
  malformed protobuf and frame bounds. Host uses OpenSSL; device uses mbedTLS.
- Firmware builds and USB uploads successfully.
- Wio hardware: US / LongFast / 906.875 MHz starts; TX_DONE observed; valid RF
  frames received; protocol handover works. Synthetic frame injection verifies
  the production receive parser, duplicate suppression and malformed rejection.
  A synthetic peer NodeInfo also exercised device mbedTLS PKI encryption through
  the deployed JS DM button and completed radio transmission; this is not a
  confirmed private-message delivery to a real peer.
- Real peer Test Peer (`!a1b2c3d4`): public text exchange and incoming private
  text confirmed. Wio Chats displayed the peer’s “Bye” in Direct. Peer public key
  remained available after reboot. Outgoing private delivery and multi-hop relay
  interoperability still require separate confirmation. TX_DONE alone confirms
  transmission, not recipient delivery.

See [MESHTASTIC_TESTING.md](MESHTASTIC_TESTING.md) for the test sequence.

## Traceroute correction (2026-10-13)

The initial client handled Routing ACK but omitted the Traceroute application
response. Endpoint replies now preserve the supplied route, append receive SNR,
and include Data.source/dest/request_id. PKI requests receive PKI replies;
channel requests use their received channel. A reply carries the original request
ID and takes the place of the standalone routing ACK for successful trace requests.
Duplicate unicast requests may receive another reply without inserting duplicate
messages. The varint parser now accepts valid ten-byte negative int32 SNR values
and rejects 64-bit overflow.

Host tests cover empty/relayed routes, negative SNR, malformed fields, overflow,
response correlation and channel decryption. On Wio, an injected 22-byte trace
request produced a 37-byte response with TX_DONE; repeating the same request
produced another response. This injection proves the endpoint code path and RF
transmission, not reception on a second device. User confirmed Wio public text
reaches their other device; the user subsequently confirmed Traceroute ACK reception.

## Private receive correction (2026-10-13)

Observed sender `!a1b2c3d4` without a known public key and failed private receive.
Peer public keys/names/contact flags now persist in NVS. Unknown PKI senders
trigger a directed NodeInfo request (throttled per sender), with two encrypted
frames retained for up to 45 seconds. Receiving NodeInfo replays those frames
through the production authenticated decrypt path. Incoming direct conversations
are selectable in Chats even without a saved contact. The Chats regression test
for a non-contact sender passes.

Initial directed NodeInfo requests timed out without a reply. Subsequently a
NodeInfo was received, the peer key survived reboot, and real private reception
was confirmed. Official NodeInfo reply
policies may suppress recent exchanges; automatic requesting cannot guarantee an
immediate reply from every firmware version. `DM Relayed` is not recipient
delivery confirmation. `@WIOMTPUBKEY` exposes only the local public key for
protocol diagnostics; private keys remain inaccessible.

Further hardware validation used an independently encrypted Python X25519 /
SHA256 / AES-CCM packet addressed to Wio (local public key only; private key
never exported). Unknown-key reception was deferred, synthetic NodeInfo supplied
the sender key, and the device authenticated/replayed the private text and
transmitted a Routing ACK. After reboot, a new encrypted packet from that sender
was immediately accepted using the saved key. The deployed Chats 1.2.3 displayed
the private text from a non-contact sender. The synthetic QA peer was forgotten
and the device rebooted to remove test messages. These are device crypto/storage/UI
checks separate from the later end-to-end incoming private test. The real peer subsequently broadcast a usable key, persisted as Test Peer.
Its incoming private test message was confirmed on the actual Chats screen. This
confirms private reception independently of the synthetic checks above.

Compatibility now decodes `want_response` from either legacy Data field 3 or
modern bitfield bit 1. Public NodeInfo overheard on the primary channel can also
be learned when addressed to another node. Diagnostic recovery commands accept
only public peer keys (`@WIOMTSETKEY NODE_HEX PUBLIC_KEY_HEX`) and permit forgetting
a stale peer (`@WIOMTFORGET NODE_HEX`). These change saved peer data.

## Role and signal controls (2026-10-13)

Radio exposes Client / Client Mute alongside region. Role updates persist separately
from the existing radio configuration, preserving identity and peer keys. Both roles
receive private messages. USB `@WIOMTROLE client|client_mute` and JS
`Pocket.setMeshtasticRole()` use the same validated setter. Unsupported roles
are rejected. RSSI/SNR are read after RX data, from one packet-status transaction;
RssiPkt supplies packet-average strength. Signal availability is separate from its
numeric value, and switching radio configurations clears the previous measurement.

Device role QA injected independent opaque PKI packets addressed to a synthetic
third node: Client Mute produced no relay; Client transmitted the expected
packet ID; a second pending packet was cancelled after a duplicate injection.
These verify the on-device scheduler and RF TX, not three-device route delivery.
Test buffers are cleared by the subsequent firmware reboot.

Final uploaded firmware observed RF RSSI -19 dBm / SNR 5.75 dB after reboot
without injected frames. Framebuffer QA confirmed active white signal bars,
gray bars when protocol is Off, and unknown measurement after restarting radio
until another packet arrives. Radio telemetry updates with the same measurement.
Client selection and real peer public key both survived firmware reboot.
