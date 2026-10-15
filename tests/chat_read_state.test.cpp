#include "ChatReadState.h"
#include <cassert>
int main() {
    ChatReadState state;
    state.view("meshtastic", false, "", true);
    assert(state.receive("meshtastic",false,"!11223344",true));
    assert(!state.unread("meshtastic",false));
    assert(!state.receive("meshtastic",true,"!11223344",true));
    assert(state.unread("meshtastic",true));
    state.read("meshtastic",true); assert(!state.unread("meshtastic",true));
    state.view("meshtastic",true,"!11223344",true);
    assert(state.receive("meshtastic",true,"!11223344",true));
    assert(!state.receive("meshtastic",true,"!55667788",true));
    assert(state.unread("meshtastic",true));
    assert(!state.receive("meshtastic",true,"!11223344",false));
    assert(!state.receive("meshcore",false,"",true));
    assert(state.unread("meshcore",false));
    assert(!state.unread("meshtastic",false));
    state.read("meshcore",false);
    state.view("meshtastic",false,"",false);
    assert(!state.receive("meshtastic",false,"",true));
    assert(state.unread("meshtastic",false));
}
