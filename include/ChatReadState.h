#pragma once
#include <cstring>
#include <cstdio>

// Read indicators belong to a protocol and tab, while popup suppression uses
// the exact currently viewed private peer. State survives closing the JS app.
class ChatReadState {
public:
    void view(const char *protocol, bool direct, const char *peer, bool visible) {
        snprintf(protocol_, sizeof(protocol_), "%s", protocol ? protocol : "");
        snprintf(peer_, sizeof(peer_), "%s", peer ? peer : "");
        direct_ = direct; visible_ = visible;
    }
    bool receive(const char *protocol, bool direct, const char *peer, bool foreground) {
        const bool seen = foreground && visible_ && !strcmp(protocol_, protocol) && direct_ == direct &&
            (!direct || (peer && peer[0] && !strcmp(peer_, peer)));
        if (!seen) unread_[family(protocol)][direct] = true;
        return seen;
    }
    bool unread(const char *protocol, bool direct) const { return unread_[family(protocol)][direct]; }
    void read(const char *protocol, bool direct) { unread_[family(protocol)][direct] = false; }
private:
    static unsigned family(const char *p) { return p && !strcmp(p, "meshcore") ? 1 : 0; }
    char protocol_[12]{}, peer_[70]{};
    bool direct_ = false, visible_ = false, unread_[2][2]{};
};
