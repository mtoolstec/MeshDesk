#include "CommunicationService.h"
#include "MeshService.h"
#include "MeshtasticService.h"
#include "Preferences.h"
#include <cassert>
#include <cstring>
MeshService meshService;
MeshtasticService meshtasticService;
int main() {
    assert(communication.begin());
    assert(meshService.starts == 0); // saved off must not initialize the radio
    assert(!communication.sendPublic("hello"));
    assert(meshService.sends == 0);
    meshtasticService.failStart = true;
    assert(!communication.select("meshtastic"));
    meshtasticService.failStart = false;
    assert(!strcmp(communication.protocol(), "off"));
    meshService.failStart = true;
    assert(!communication.select("meshcore"));
    assert(Preferences::protocol == "off");
    meshService.failStart = false;
    assert(communication.select("meshcore"));
    assert(communication.ready());
    assert(communication.sendPublic("hello"));
    assert(!communication.select("invalid"));
    assert(communication.ready()); // rejected selections must not interrupt the active protocol
    assert(communication.select("off"));
    assert(!meshService.online);
    const auto polls = meshService.polls;
    communication.poll();
    assert(meshService.polls == polls);
    assert(Preferences::protocol == "off");
    assert(communication.select("meshcore"));
    assert(communication.ready());
    meshtasticService.failStart = true;
    assert(!communication.select("meshtastic")); // rollback resumes MeshCore
    assert(meshService.online && !strcmp(communication.protocol(), "meshcore"));
    meshtasticService.failStart = false;
    assert(communication.select("meshtastic"));
    assert(!meshService.online && meshtasticService.online);
    assert(Preferences::protocol == "meshtastic");
    assert(communication.sendPublic("test"));
    assert(meshtasticService.sends == 1);
    communication.poll(); assert(meshtasticService.polls == 1);
    meshtasticService.failStop = true;
    assert(!communication.select("meshcore"));
    assert(!strcmp(communication.protocol(), "meshtastic"));
    meshtasticService.failStop = false;
    assert(communication.select("off"));
    assert(!strcmp(communication.configurationProtocol(), "meshtastic"));
    assert(!meshtasticService.online);
    assert(communication.select("meshcore"));
}
