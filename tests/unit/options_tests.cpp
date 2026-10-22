// Reading meshcentral:// addresses.

#include "check.h"

#include "meshcentral/options.h"

using namespace meshcentral;

namespace {

const std::string pin_hex = "52:D9:2B:98:A0:3D:E2:81:7C:58:43:8F:CE:37:68:BB:C7:D3:D7:D4:80:00:3B:E1:98:8B:0E:68:97:46:8D:6F";

}  // namespace

TEST(options_full_address) {
    std::string error;
    auto o = parse_options("meshcentral://helpdesk@mesh.example.com:8443/Reception-PC?group=Office&display=2&quality=70"
                           "&fps=20&thumbprint=" + pin_hex,
                           error);
    REQUIRE(o);
    CHECK_EQ(o->server, std::string("mesh.example.com"));
    CHECK_EQ(o->url_host, std::string("mesh.example.com"));
    CHECK_EQ(o->port, 8443);
    CHECK_EQ(o->user, std::string("helpdesk"));
    CHECK_EQ(o->device, std::string("Reception-PC"));
    CHECK(!o->device_is_id);
    CHECK_EQ(o->group, std::string("Office"));
    REQUIRE(o->display);
    CHECK_EQ(*o->display, 2);
    CHECK_EQ(o->quality, 70);
    CHECK_EQ(o->fps, 20);
    CHECK_EQ(o->frame_timer_ms(), 50);
    REQUIRE(o->thumbprint);
    CHECK_EQ(format_fingerprint(*o->thumbprint), pin_hex);
}

TEST(options_defaults) {
    std::string error;
    auto o = parse_options("meshcentral://mesh.example.com/PC", error);
    REQUIRE(o);
    CHECK_EQ(o->port, 443);
    CHECK(o->user.empty());
    CHECK(o->group.empty());
    CHECK(!o->display);
    CHECK_EQ(o->quality, 50);
    CHECK_EQ(o->fps, 10);
    CHECK_EQ(o->frame_timer_ms(), 100);
    CHECK(!o->thumbprint);
}

TEST(options_node_ids_and_names) {
    std::string error;
    // Casement percent-encodes the path; node ids contain / $ and @.
    auto o = parse_options("meshcentral://u@h/node%2F%2FXRZpPkeU7n8FJpy8p7lEHxRp%24HlkQRgiwDuSHtQ8pTCezL49afde", error);
    REQUIRE(o);
    CHECK(o->device_is_id);
    CHECK_EQ(o->device, std::string("node//XRZpPkeU7n8FJpy8p7lEHxRp$HlkQRgiwDuSHtQ8pTCezL49afde"));
    // Typed by hand, unencoded.
    o = parse_options("meshcentral://u@h/node//abc@def$", error);
    REQUIRE(o);
    CHECK(o->device_is_id);
    CHECK_EQ(o->device, std::string("node//abc@def$"));
    // A device in a DNS-named domain.
    CHECK(is_node_id("node/customer1/abc"));
    // Names with spaces and non-ASCII letters.
    o = parse_options("meshcentral://u@h/Empfang%20Gr%C3%BC%C3%9Fe", error);
    REQUIRE(o);
    CHECK(!o->device_is_id);
    CHECK_EQ(o->device, std::string("Empfang Grüße"));
    CHECK(!is_node_id("node//"));
    CHECK(!is_node_id("node/a"));
    CHECK(!is_node_id("nodes//abc"));
    CHECK(!is_node_id("node//abc def"));
    CHECK(!parse_options("meshcentral://u@h/node//has space", error));
}

TEST(options_ipv6_and_display_all) {
    std::string error;
    auto o = parse_options("meshcentral://u@[fd00::5]:10443/PC?display=all", error);
    REQUIRE(o);
    CHECK_EQ(o->server, std::string("fd00::5"));
    CHECK_EQ(o->url_host, std::string("[fd00::5]"));
    CHECK_EQ(o->port, 10443);
    REQUIRE(o->display);
    CHECK_EQ(*o->display, 65535);
    o = parse_options("meshcentral://u@h/PC?display=65535&quality=&fps=", error);
    REQUIRE(o);
    CHECK_EQ(*o->display, 65535);
    CHECK_EQ(o->quality, 50);
    CHECK_EQ(o->fps, 10);
}

TEST(options_bad_values) {
    std::string error;
    CHECK(!parse_options("meshcentral://u@h", error));
    CHECK(error.find("which device") != std::string::npos);
    CHECK(!parse_options("meshcentral://u@h/", error));
    CHECK(!parse_options("rustdesk://u@h/PC", error));
    CHECK(!parse_options("meshcentral://u@h/PC?quality=0", error));
    CHECK(error.find("1 to 100") != std::string::npos);
    CHECK(!parse_options("meshcentral://u@h/PC?quality=101", error));
    CHECK(!parse_options("meshcentral://u@h/PC?quality=high", error));
    CHECK(!parse_options("meshcentral://u@h/PC?fps=51", error));
    CHECK(error.find("1 to 50") != std::string::npos);
    CHECK(!parse_options("meshcentral://u@h/PC?display=-1", error));
    CHECK(!parse_options("meshcentral://u@h/PC?display=70000", error));
    CHECK(!parse_options("meshcentral://u@h:0/PC", error));
}

TEST(options_thumbprints) {
    std::string error;
    auto t = parse_thumbprint("52d92b98a03de2817c58438fce3768bbc7d3d7d480003be1988b0e6897468d6f", error);
    REQUIRE(t);
    CHECK_EQ(format_fingerprint(*t), pin_hex);
    t = parse_thumbprint(" 52 D9 2B 98 A0 3D E2 81 7C 58 43 8F CE 37 68 BB C7 D3 D7 D4 80 00 3B E1 98 8B 0E 68 97 46 8D 6F ",
                         error);
    REQUIRE(t);
    CHECK_EQ(format_fingerprint(*t), pin_hex);
    // A SHA-1 thumbprint, as Windows' certificate dialog shows it.
    CHECK(!parse_thumbprint("a9 99 3e 36 47 06 81 6a ba 3e 25 71 78 50 c2 6c 9c d0 d8 9d", error));
    CHECK(error.find("SHA-1") != std::string::npos);
    CHECK(!parse_thumbprint("52:D9:2B", error));
    CHECK(!parse_thumbprint("zz", error));
    CHECK(!parse_options("meshcentral://u@h/PC?thumbprint=52:D9", error));
}
