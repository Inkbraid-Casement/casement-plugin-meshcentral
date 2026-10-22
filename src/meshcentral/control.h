// MeshCentral's control channel (control.ashx): JSON messages with an
// "action" member, as MeshCentral's web client and meshctrl.js use them.
// This file builds the requests and reads the answers; the session does the
// talking.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "casement/protocol.h"

namespace meshcentral::control {

using casement::json;

/// The login message for in-band authentication ("x-meshauth: *").
json user_auth(std::string_view user, std::string_view password);

json meshes_request();
json nodes_request();
json authcookie_request();

/// Asks the agent to join a relay for remote desktop, the way the web
/// client does. `response_id` makes the server say whether it could pass
/// the request on.
json tunnel_request(std::string_view node_id, std::string_view tunnel_id, std::string_view relay_cookie,
                    std::string_view response_id);

/// Path and query of our side of the relay.
std::string relay_path(std::string_view node_id, std::string_view tunnel_id, std::string_view cookie);

/// Clipboard requests to the agent. Tag 3 marks automatic polling, which
/// agents don't write into the device's event log.
json getclip_request(std::string_view node_id);
json setclip_request(std::string_view node_id, std::string_view text);

json pong();

/// Why the server ended the login ({"action":"close",…}), as a final state.
struct Refusal {
    casement::State state = casement::State::failed;
    std::string text;
};
Refusal refusal(const json& close);

struct Group {
    std::string id;  // mesh/<domain>/<id>
    std::string name;
};

struct Device {
    std::string id;  // node/<domain>/<id>
    std::string name;
    std::string group_id;
    /// Connectivity bits: 1 agent, 2 Intel AMT CIRA, 4 Intel AMT local.
    int conn = 0;
    bool has_agent = false;
    /// The agent's architecture number (MeshCentral's meshAgentsArchitectureNumbers).
    int agent_id = 0;
    /// Agent capabilities: bit 1 is remote desktop. Missing on old agents.
    std::optional<int> agent_caps;
    std::string os;
};

std::vector<Group> parse_meshes(const json& msg);
std::vector<Device> parse_nodes(const json& msg);

/// What the address asks for.
struct Wanted {
    std::string device;
    bool device_is_id = false;
    std::string group;
};

/// The device the address names, or a sentence saying why there isn't one.
struct Found {
    std::optional<Device> device;
    std::string error;
};
Found find_device(const std::vector<Device>& devices, const std::vector<Group>& groups, const Wanted& wanted);

/// Why the device can't show its desktop now, or empty.
std::string desktop_problem(const Device& device);

/// Whether the agent runs on Windows.
bool windows_agent(int agent_id);

/// The failure text when the agent asked the person at the device to allow
/// the session and that didn't happen: `reason` is the agent's console
/// message ("denied", "timeout", or why it couldn't ask).
std::string consent_refused(std::string_view device, std::string_view reason);

}  // namespace meshcentral::control
