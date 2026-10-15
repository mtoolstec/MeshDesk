# Meshtastic two-device test

Configure the other device to match the Wio:

| Setting | Value |
| --- | --- |
| Region | US (US915) |
| Modem | LongFast |
| Primary channel | Default LongFast |
| Channel PSK | Default `AQ==` |
| Frequency override | Disabled / 0 |
| Default center frequency | 906.875 MHz |
| Channel encryption | Classic PSK (not channel AEAD) |

Open **Radio** on Wio and select Meshtastic / LongFast / US. The settings persist.
Use Chats for public and direct messages, Contacts for peer selection, and Radio
for protocol settings and advertisements. The standalone Mesh Test app has been
removed.

1. Send a public message from the other device and confirm it appears in Chats.
2. Reply from Wio Chats and confirm delivery on the other device. A local TX
   counter alone does not establish delivery.
3. Exchange NodeInfo/advertisements and allow public keys to be learned.
4. Select the peer in Contacts, send a direct message, and test the private reply.
   An unknown sender triggers key exchange; private sending requires its key.
5. Run Traceroute to Wio from the other device and check the returned path. Wio
   does not provide full intermediate route enrichment.
6. Reboot and verify the protocol, region, modem, peers, and keys persist; repeat
   public and private message tests.

If a test fails, record the other device's firmware version, exact channel
settings, TX/RX/ACK counters and whether public or private messaging failed.
Do not publish private keys or custom channel PSKs in logs/screenshots.

USB diagnostics: `@WIOMTSTATUS`, `@WIOADVERTISE`, `@WIOCOMMSEND TEXT`,
`@WIOPROTOCOL meshtastic`. `@WIOMTINJECT HEX` is parser testing only and must
never be counted as a successful RF reception. Reboot clears test fixtures.

## Client role and signal checks

In Radio, select Client or Client Mute next to region; reboot and verify the
selection persists. Both roles must receive direct messages. Client Mute must
not relay a third node’s packet. Client should relay an eligible classic packet
with one fewer remaining hop and cancel a pending relay after overhearing a
duplicate. Opaque PKI packets to another node can relay without its private key.
Intermediate Traceroute path enrichment and full next-hop routing are not implemented.

Signal bars show the last received packet’s average RSSI for the active protocol.
After reboot/frequency change, bars are gray until a valid measurement arrives.
Use `@WIOMTSTATUS` for signalKnown/RSSI/SNR and compare with the status bar.
Protocol-off must show gray bars.
