// The control channel's messages and finding the device an address names.

#include "check.h"

#include <stdexcept>

#include "casement/text.h"
#include "meshcentral/control.h"

using namespace meshcentral::control;
using casement::json;
using casement::State;

namespace {

std::string decode64(const json& value) {
    auto bytes = casement::base64_decode(value.get<std::string>());
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string();
}

/// What MeshCentral 1.2.5 sends for a user who can see two groups.
const json nodes_answer = json::parse(R"({
  "action": "nodes",
  "nodes": {
    "mesh//office": [
      {"_id": "node//aaa", "name": "Reception-PC", "conn": 1, "pwr": 1, "osdesc": "Windows 11 Pro",
       "agent": {"ver": 0, "id": 4, "caps": 15, "core": "x"}},
      {"_id": "node//bbb", "name": "Laptop", "conn": 0, "agent": {"id": 6, "caps": 31}},
      {"_id": "node//ccc", "name": "Printer", "conn": 0}
    ],
    "mesh//lab": [
      {"_id": "node//ddd", "name": "reception-pc", "conn": 1, "agent": {"id": 6, "caps": 31}},
      {"_id": "node//eee", "name": "Laptop", "conn": 1, "agent": {"id": 6, "caps": 2}},
      {"name": "no id"},
      "junk"
    ],
    "mesh//empty": "junk"
  }
})");

const json meshes_answer = json::parse(R"({
  "action": "meshes",
  "meshes": [
    {"_id": "mesh//office", "name": "Office", "mtype": 2},
    {"_id": "mesh//lab", "name": "Lab", "mtype": 2},
    {"name": "no id"}
  ]
})");

const Device& by_id(const std::vector<Device>& devices, std::string_view id) {
    for (const auto& d : devices)
        if (d.id == id) return d;
    throw std::runtime_error("no device " + std::string(id));
}

Found find(std::string device, std::string group = {}) {
    auto devices = parse_nodes(nodes_answer);
    auto groups = parse_meshes(meshes_answer);
    bool id = device.starts_with("node/");
    return find_device(devices, groups, {device, id, group});
}

}  // namespace

TEST(control_login_message) {
    json m = user_auth("helpdesk", "Grüße:,secret");
    CHECK_EQ(m["action"].get<std::string>(), std::string("userAuth"));
    CHECK_EQ(decode64(m["username"]), std::string("helpdesk"));
    CHECK_EQ(decode64(m["password"]), std::string("Grüße:,secret"));
    CHECK(!m.contains("token"));
}

TEST(control_tunnel_messages) {
    json t = tunnel_request("node//abc$@x", "k3j4h5", "R$ck@=", "casement-tunnel");
    CHECK_EQ(t["action"].get<std::string>(), std::string("msg"));
    CHECK_EQ(t["type"].get<std::string>(), std::string("tunnel"));
    CHECK_EQ(t["nodeid"].get<std::string>(), std::string("node//abc$@x"));
    CHECK_EQ(t["usage"].get<int>(), 2);
    CHECK_EQ(t["responseid"].get<std::string>(), std::string("casement-tunnel"));
    // As agent-redir-ws builds it; the server checks the node id in it.
    CHECK_EQ(t["value"].get<std::string>(), std::string("*/meshrelay.ashx?p=2&nodeid=node//abc$@x&id=k3j4h5&rauth=R$ck@="));
    CHECK_EQ(relay_path("node//abc$@x", "k3j4h5", "C$o@k+ie="),
             std::string("/meshrelay.ashx?browser=1&p=2&nodeid=node%2F%2Fabc%24%40x&id=k3j4h5&auth=C%24o%40k%2Bie%3D"));
}

TEST(control_clipboard_messages) {
    json g = getclip_request("node//abc");
    CHECK_EQ(g.dump(), std::string(R"({"action":"msg","nodeid":"node//abc","tag":3,"type":"getclip"})"));
    json s = setclip_request("node//abc", "Grüße\nzwei Zeilen");
    CHECK_EQ(s["type"].get<std::string>(), std::string("setclip"));
    CHECK_EQ(s["data"].get<std::string>(), std::string("Grüße\nzwei Zeilen"));
    CHECK_EQ(pong().dump(), std::string(R"({"action":"pong"})"));
}

TEST(control_login_refusals) {
    auto two_factor = refusal(json::parse(R"({"action":"close","cause":"noauth","msg":"tokenrequired","email2fa":false})"));
    CHECK(two_factor.state == State::failed);
    CHECK(two_factor.text.starts_with("This account uses two-factor authentication; use a login token"));
    auto wrong = refusal(json::parse(R"({"action":"close","cause":"noauth","msg":"noauth-2c"})"));
    CHECK(wrong.state == State::auth_failed);
    CHECK(refusal(json::parse(R"({"action":"close","cause":"noauth","msg":"noauth-2d"})")).state == State::auth_failed);
    CHECK(refusal(json::parse(R"({"action":"close","cause":"notools","msg":"notools"})")).state == State::failed);
    CHECK(refusal(json::parse(R"({"action":"close","cause":"emailvalidation","msg":"emailvalidationrequired"})")).state ==
          State::failed);
    CHECK(refusal(json::parse(R"({"action":"close","cause":"banned","msg":"banned-1"})")).state == State::failed);
    auto other = refusal(json::parse(R"({"action":"close","cause":"strange","msg":"thing"})"));
    CHECK(other.state == State::failed);
    CHECK(other.text.find("strange: thing") != std::string::npos);
}

TEST(control_parses_lists) {
    auto groups = parse_meshes(meshes_answer);
    REQUIRE(groups.size() == 2);
    CHECK_EQ(groups[1].name, std::string("Lab"));
    auto devices = parse_nodes(nodes_answer);
    REQUIRE(devices.size() == 5);
    const auto& pc = by_id(devices, "node//aaa");
    CHECK_EQ(pc.id, std::string("node//aaa"));
    CHECK_EQ(pc.group_id, std::string("mesh//office"));
    CHECK_EQ(pc.conn, 1);
    CHECK(pc.has_agent);
    CHECK_EQ(pc.agent_id, 4);
    CHECK(pc.agent_caps == std::optional<int>(15));
    CHECK_EQ(pc.os, std::string("Windows 11 Pro"));
    CHECK(!by_id(devices, "node//ccc").has_agent);
    CHECK(parse_nodes(json::parse(R"({"action":"nodes"})")).empty());
    CHECK(parse_meshes(json::parse(R"({"action":"meshes","meshes":{}})")).empty());
}

TEST(control_finds_devices_by_name) {
    // Exact names first; the case only matters when it tells devices apart.
    auto f = find("Reception-PC");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//aaa"));
    f = find("RECEPTION-PC", "Lab");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//ddd"));
    f = find("printer");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//ccc"));

    // Two devices of the same name: the group or the node id decides.
    f = find("Laptop");
    CHECK(!f.device);
    CHECK(f.error.find("There are 2 devices named “Laptop”") != std::string::npos);
    CHECK(f.error.find("node//bbb (“Office”)") != std::string::npos);
    CHECK(f.error.find("node//eee (“Lab”)") != std::string::npos);
    f = find("Laptop", "Lab");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//eee"));
    f = find("Laptop", "office");  // group names case-insensitively too
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//bbb"));
    f = find("Laptop", "mesh//lab");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//eee"));

    f = find("Nothing");
    CHECK(!f.device);
    CHECK_EQ(f.error, std::string("There's no device named “Nothing” that this account can see."));
    f = find("Printer", "Lab");
    CHECK(!f.device);
    CHECK(f.error.find("in the device group “Lab”") != std::string::npos);
    f = find("Printer", "Garage");
    CHECK(!f.device);
    CHECK_EQ(f.error, std::string("There's no device group named “Garage” that this account can see."));
    f = find("Printer", "mesh//nope");
    CHECK(!f.device);
}

TEST(control_finds_devices_by_the_id_the_web_site_shows) {
    // The device page shows "Node ID" without "node//".
    auto f = find("eee");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//eee"));
    f = find("eee", "Office");
    CHECK(!f.device);
    // A name still comes first.
    f = find("Laptop", "Lab");
    REQUIRE(f.device);
    CHECK_EQ(f.device->id, std::string("node//eee"));
}

TEST(control_finds_devices_by_id) {
    auto f = find("node//eee");
    REQUIRE(f.device);
    CHECK_EQ(f.device->name, std::string("Laptop"));
    f = find("node//eee", "Lab");
    CHECK(f.device);
    f = find("node//eee", "Office");
    CHECK(!f.device);
    CHECK(f.error.find("isn't in the device group “Office”") != std::string::npos);
    f = find("node//zzz");
    CHECK(!f.device);
    CHECK(f.error.find("no device with the node id node//zzz") != std::string::npos);
}

TEST(control_desktop_problems) {
    auto devices = parse_nodes(nodes_answer);
    CHECK(desktop_problem(by_id(devices, "node//aaa")).empty());
    CHECK(desktop_problem(by_id(devices, "node//bbb")).find("is offline") != std::string::npos);
    CHECK(desktop_problem(by_id(devices, "node//ccc")).find("no MeshCentral agent") != std::string::npos);
    CHECK(desktop_problem(by_id(devices, "node//ddd")).empty());
    CHECK(desktop_problem(by_id(devices, "node//eee")).find("doesn't offer remote desktop") != std::string::npos);
    // Agents too old to report capabilities get the benefit of the doubt.
    Device old;
    old.name = "old";
    old.has_agent = true;
    old.conn = 1;
    CHECK(desktop_problem(old).empty());
}

TEST(control_windows_agents) {
    for (int id : {1, 2, 3, 4, 21, 22, 34, 42, 43}) CHECK(windows_agent(id));
    for (int id : {0, 5, 6, 16, 26, 29, 30}) CHECK(!windows_agent(id));
}

TEST(control_consent_answers) {
    CHECK_EQ(consent_refused("Desk", "denied"), std::string("The person at “Desk” didn't allow the session."));
    CHECK_EQ(consent_refused("Desk", "timeout"),
             std::string("Nobody at “Desk” answered the request to allow the session."));
    CHECK_EQ(consent_refused("Desk", "TIMEOUT"),
             std::string("Nobody at “Desk” answered the request to allow the session."));
    // Linux agents without a way to show the question say so.
    CHECK_EQ(consent_refused("Desk", "Unable to create dialog box"),
             std::string("“Desk” couldn't ask its user to allow the session: Unable to create dialog box"));
    CHECK_EQ(consent_refused("Desk", ""), std::string("“Desk” didn't allow the session."));
}
