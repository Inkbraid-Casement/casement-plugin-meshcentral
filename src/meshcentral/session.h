// A Casement tab showing one MeshCentral device: the control channel for
// login, device lookup and clipboard, and a relay to the device's agent for
// the desktop.

#pragma once

#include <memory>

#include "casement/plugin.h"

namespace meshcentral {

/// Starts a session for Casement's `connect`. Problems with the address or
/// the credentials end the process with the matching state.
std::unique_ptr<casement::Session> start_session(casement::Host& host, const casement::Connect& connect);

}  // namespace meshcentral
