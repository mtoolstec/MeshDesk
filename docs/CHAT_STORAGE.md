# SD chat storage

Firmware captures messages even when Chats is closed. `/apps/chats` contains
application code; persistent state is separate:

```text
/appdata/chats/
  contacts.json
  contacts.json.bak
  messages/00000001.log
  messages/00000002.log
  legacy-import.done
```

Contacts include protocol, stable node ID, display name, saved-contact flag and
public key. The private identity key remains in NVS. Contacts use versioned JSON,
a generation counter and CRC32; a verified backup is used if the current file is
invalid. Messages are locally decoded text and data, stored in plaintext.

## Bounded log

The append log avoids a database dependency on the embedded device. Each record
is 512 bytes, with magic, CRC32, stable ID, timestamp, protocol, peer ID/name,
packet ID, target ID, port, flags and up to 240 payload bytes. Deletion appends a
tombstone. Segments hold 128 records; at most 16 segments are retained (about
1 MiB, up to 2048 records including deletion records). The oldest segment rotates
out when full. Invalid or torn records are skipped; earlier valid records remain.
The index is rebuilt in PSRAM at startup. Eight records may wait in RAM while
writes are drained; this queue is bounded, so prolonged SD failure can lose new
messages. Chats shows pending writes and SD errors. Data captured before this
feature existed only in RAM cannot be recovered after a reboot.

## PocketJS

- `chatHistory({limit, before, direct, peerId, includeData})`: current protocol,
  chronological page, newest page by default; maximum 50 rows. Use the oldest
  row's ID as `before` to retrieve earlier messages. Default excludes data ports.
- `chatPeers()`: current protocol's contacts/nodes with stable IDs and names.
- `chatStorage()`: readiness, pending count, retained message count, path and error.
- `deleteChatMessage(id)`: persistent deletion by stable record ID.
- `readDataFile(name)` / `writeDataFile(name, text)`: per-app files under
  `/appdata/<app-id>`, up to 64 KiB. Use custom file names; the files listed above
  belong to firmware and should not be overwritten by application code.

Chats uses a vertically scrolling message area with fixed tabs and composer.
It initially reads the latest 50 messages. At the top, Load more prepends up to
50 earlier messages and preserves the current reading position. Nine recycled
rows keep widget and callback counts bounded as history grows. Incoming messages
follow the bottom only while the reader is already there.
The scrollbar sits at the right screen edge, with separate space beyond the text.
Messages wrap at the available text width and each row grows to fit its full
content. Font measurements determine row heights, with compact line spacing.
History prepends preserve the reading position using measured pixel heights.
Protocol data remains stored and accessible through the SDK; the chat screen only shows text.
The menu provides jump to latest, abbreviated names, notification sounds, and
clear-current-chat with confirmation. Direct message rows show only message text;
the selected peer appears once in the header. Short names show the last four
hexadecimal characters of the stable node ID (for example `!a1b2c3d4` → `c3d4`).
Legacy peers without a hexadecimal ID keep their display name. The choice persists in
`/appdata/chats/preferences.json`; it does not change the transmitted node name.
Clearing deletes only text in the selected protocol and public/private chat,
including older pages; contacts, other chats and protocol data are retained.
Deletion drains in small batches and stops if the protocol changes.
Message long press opens copy/delete actions. Switching protocol filters history.

## Unread indicators and foreground notifications

Incoming text in the currently visible Chats conversation is treated as read
and is not added to Notification Center or the system banner. Private messages
match the stable peer ID. Other incoming text keeps normal notifications and
sets the Groups or Direct dot. Selecting the corresponding tab clears its dot;
selecting an already active tab keeps the same tab selected. Unread flags are
separate per protocol and remain in RAM while the app is closed, until reboot.
Screen sleep and switching away from Chats restore normal notifications.

`Pocket.setChatView(direct, peerId, visible)` declares the foreground conversation
from `/apps/chats`. `Pocket.chatUnread()` returns `{groups, direct}`;
`Pocket.markChatRead(direct)` clears one tab. These do not erase message history.

## Launch from Contacts

Contacts queues the selected stable peer ID and protocol before navigating.
`Pocket.takeChatTarget()` consumes that ID once from Chats startup; Chats selects
the matching private peer before rendering. The native fallback applies the
selection only after its controls exist. Contacts never manipulates the JS
chat page through native widget pointers.

## Notification sound types

Names and Sound share one row in the options panel. Sound opens a panel of the
same size, with Default, Chime, Double, Soft and Silent; selecting a type saves
it and previews it. Groups and Direct use independent choices, across protocols.
Firmware persists these two device-wide choices in NVS (`mesh-ui/snd-ch-style`
and `snd-dm-style`), so notifications also use them while Chats is closed.
The Settings master/channel/direct sound switches and device volume still apply
to incoming messages. Preview bypasses the switches to allow auditioning.
