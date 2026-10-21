#include "meshcentral/control.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "casement/address.h"
#include "casement/text.h"

namespace meshcentral::control {

using casement::get_int;
using casement::get_string;

namespace {

std::string b64(std::string_view s) {
    return casement::base64_encode(std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const std::string* group_name(const std::vector<Group>& groups, const std::string& id) {
    for (const auto& g : groups)
        if (g.id == id) return &g.name;
    return nullptr;
}

}  // namespace

json user_auth(std::string_view user, std::string_view password) {
    return json{{"action", "userAuth"}, {"username", b64(user)}, {"password", b64(password)}};
}

json meshes_request() { return json{{"action", "meshes"}}; }
json nodes_request() { return json{{"action", "nodes"}}; }
json authcookie_request() { return json{{"action", "authcookie"}}; }

json tunnel_request(std::string_view node_id, std::string_view tunnel_id, std::string_view relay_cookie,
                    std::string_view response_id) {
    // The same form as the web client's (agent-redir-ws): the server checks
    // that the node id in the URL matches and replaces "*" with its own
    // address before passing the request to the agent.
    std::string value = std::format("*/meshrelay.ashx?p=2&nodeid={}&id={}&rauth={}", node_id, tunnel_id, relay_cookie);
    return json{{"action", "msg"}, {"type", "tunnel"}, {"nodeid", node_id}, {"value", value},
                {"usage", 2},      {"responseid", response_id}};
}

std::string relay_path(std::string_view node_id, std::string_view tunnel_id, std::string_view cookie) {
    return std::format("/meshrelay.ashx?browser=1&p=2&nodeid={}&id={}&auth={}", casement::percent_encode(node_id),
                       casement::percent_encode(tunnel_id), casement::percent_encode(cookie));
}

json getclip_request(std::string_view node_id) {
    return json{{"action", "msg"}, {"type", "getclip"}, {"nodeid", node_id}, {"tag", 3}};
}

json setclip_request(std::string_view node_id, std::string_view text) {
    return json{{"action", "msg"}, {"type", "setclip"}, {"nodeid", node_id}, {"data", text}};
}

json pong() { return json{{"action", "pong"}}; }

Refusal refusal(const json& close) {
    using casement::State;
    std::string cause = get_string(close, "cause"), msg = get_string(close, "msg");
    if (cause == "noauth") {
        if (msg == "tokenrequired")
            return {State::failed,
                    "This account uses two-factor authentication; use a login token. MeshCentral makes one on "
                    "its My Account page, with “Create login token”."};
        if (msg == "nokey")
            return {State::failed, "The server only accepts logins through a URL with a login key, which the "
                                   "plugin doesn't support."};
        return {State::auth_failed, "The user name or password is wrong."};
    }
    if (cause == "notools")
        return {State::failed, "This account may not use tools such as this plugin: it has MeshCentral's "
                               "“No Tools (MeshCmd/Router)” restriction."};
    if (cause == "emailvalidation")
        return {State::failed, "MeshCentral wants this account's email address verified first. Log in to the "
                               "MeshCentral web site once to do that."};
    if (cause == "banned")
        return {State::failed, "The server doesn't accept connections from this computer's address."};
    if (cause == "expired") return {State::auth_failed, "The login has expired."};
    return {State::failed, std::format("The server refused the login ({}{}{}).", cause.empty() ? "no reason" : cause,
                                       msg.empty() ? "" : ": ", msg)};
}

std::vector<Group> parse_meshes(const json& msg) {
    std::vector<Group> out;
    auto it = msg.find("meshes");
    if (it == msg.end() || !it->is_array()) return out;
    for (const auto& m : *it)
        if (m.is_object() && m.contains("_id")) out.push_back({get_string(m, "_id"), get_string(m, "name")});
    return out;
}

std::vector<Device> parse_nodes(const json& msg) {
    std::vector<Device> out;
    auto it = msg.find("nodes");
    if (it == msg.end() || !it->is_object()) return out;
    for (const auto& [group, list] : it->items()) {
        if (!list.is_array()) continue;
        for (const auto& n : list) {
            if (!n.is_object()) continue;
            Device d;
            d.id = get_string(n, "_id");
            if (d.id.empty()) continue;
            d.name = get_string(n, "name");
            d.group_id = group;
            d.conn = static_cast<int>(get_int(n, "conn"));
            d.os = get_string(n, "osdesc");
            if (auto a = n.find("agent"); a != n.end() && a->is_object()) {
                d.has_agent = true;
                d.agent_id = static_cast<int>(get_int(*a, "id"));
                if (auto c = a->find("caps"); c != a->end() && c->is_number()) d.agent_caps = static_cast<int>(get_int(*a, "caps"));
            }
            out.push_back(std::move(d));
        }
    }
    return out;
}

Found find_device(const std::vector<Device>& devices, const std::vector<Group>& groups, const Wanted& wanted) {
    // The groups to look in; empty for all.
    std::vector<std::string> group_ids;
    if (!wanted.group.empty()) {
        if (wanted.group.starts_with("mesh/")) {
            bool known = std::any_of(groups.begin(), groups.end(), [&](const Group& g) { return g.id == wanted.group; });
            if (!known) return {std::nullopt, std::format("There's no device group with the id “{}” that this account can see.", wanted.group)};
            group_ids.push_back(wanted.group);
        } else {
            for (const auto& g : groups)
                if (g.name == wanted.group) group_ids.push_back(g.id);
            if (group_ids.empty())
                for (const auto& g : groups)
                    if (lower(g.name) == lower(wanted.group)) group_ids.push_back(g.id);
            if (group_ids.empty())
                return {std::nullopt, std::format("There's no device group named “{}” that this account can see.", wanted.group)};
        }
    }
    auto in_groups = [&](const Device& d) {
        return group_ids.empty() || std::find(group_ids.begin(), group_ids.end(), d.group_id) != group_ids.end();
    };
    std::string where = wanted.group.empty() ? std::string() : std::format(" in the device group “{}”", wanted.group);

    if (wanted.device_is_id) {
        for (const auto& d : devices) {
            if (d.id != wanted.device) continue;
            if (!in_groups(d))
                return {std::nullopt, std::format("The device {} isn't{}.", wanted.device, where)};
            return {d, {}};
        }
        return {std::nullopt, std::format("There's no device with the node id {} that this account can see.", wanted.device)};
    }

    std::vector<const Device*> matches;
    for (const auto& d : devices)
        if (in_groups(d) && d.name == wanted.device) matches.push_back(&d);
    if (matches.empty())
        for (const auto& d : devices)
            if (in_groups(d) && lower(d.name) == lower(wanted.device)) matches.push_back(&d);
    if (matches.empty() && wanted.device.find('/') == std::string::npos) {
        // MeshCentral's web site shows a node id without its "node/<domain>/"
        // part; that is taken as the id too.
        std::string tail = "/" + wanted.device;
        for (const auto& d : devices)
            if (in_groups(d) && d.id.starts_with("node/") && d.id.ends_with(tail)) matches.push_back(&d);
    }
    if (matches.empty())
        return {std::nullopt, std::format("There's no device named “{}”{} that this account can see.", wanted.device, where)};
    if (matches.size() == 1) return {*matches[0], {}};

    std::string list;
    for (std::size_t i = 0; i < matches.size() && i < 5; ++i) {
        if (i > 0) list += ", ";
        list += matches[i]->id;
        if (const std::string* g = group_name(groups, matches[i]->group_id)) list += std::format(" (“{}”)", *g);
    }
    if (matches.size() > 5) list += ", …";
    return {std::nullopt,
            std::format("There are {} devices named “{}”{}. Add the device group to the address, or use the node id "
                        "instead of the name: {}.",
                        matches.size(), wanted.device, where, list)};
}

std::string desktop_problem(const Device& d) {
    if (!d.has_agent) return std::format("“{}” has no MeshCentral agent, so there is no desktop to show.", d.name);
    if ((d.conn & 1) == 0)
        return std::format("“{}” is offline: its MeshCentral agent isn't connected to the server.", d.name);
    if (d.agent_caps && (*d.agent_caps & 1) == 0)
        return std::format("The MeshCentral agent on “{}” doesn't offer remote desktop.", d.name);
    return {};
}

std::string consent_refused(std::string_view device, std::string_view reason) {
    if (reason == "denied") return std::format("The person at “{}” didn't allow the session.", device);
    if (reason == "timeout" || reason == "TIMEOUT")
        return std::format("Nobody at “{}” answered the request to allow the session.", device);
    if (reason.empty()) return std::format("“{}” didn't allow the session.", device);
    return std::format("“{}” couldn't ask its user to allow the session: {}", device, reason);
}

bool windows_agent(int agent_id) {
    // MeshCentral's meshAgentsArchitectureNumbers: Windows consoles and
    // services on x86, x64 and ARM64, MinCore builds, and MeshCentral's own
    // Windows helper program (34 and 10006).
    switch (agent_id) {
        case 1: case 2: case 3: case 4: case 21: case 22: case 34: case 42: case 43:
        case 10003: case 10004: case 10006:
            return true;
        default:
            return false;
    }
}

}  // namespace meshcentral::control
