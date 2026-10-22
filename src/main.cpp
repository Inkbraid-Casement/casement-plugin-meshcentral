// casement-meshcentral: opens the desktops of MeshCentral agents in Casement.

#include "casement/plugin.h"
#include "meshcentral/session.h"

int main() {
    casement::Info info{"MESHCENTRAL", "MeshCentral", CASEMENT_PLUGIN_VERSION, {"password", "clipboard", "cursor"}, 1, 1};
    return casement::run(info, meshcentral::start_session);
}
