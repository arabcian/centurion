// Unit test for the firewall / lookup input checks (netrules.cpp). Plain asserts, like traffic_test.cpp.
#include "../src/netrules.h"

#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
static QString s(const char *t) { return QString::fromUtf8(t); }

int main() {
    CHECK(expandPorts(s("22, 80,8000-8002,80"), 64) == QList<int>({22, 80, 8000, 8001, 8002}));
    CHECK(expandPorts(s("1-65"), 64).isEmpty());
    CHECK(expandPorts(s("1-64"), 64).size() == 64);
    for (const char *bad : {"0", "65536", "9-1", "x", "1-2-3"}) CHECK(expandPorts(s(bad), 64).isEmpty());

    CHECK(validPortList(QString()) && validPortList(s("80, 443,1000-65535")));
    CHECK(!validPortList(s("80;")) && !validPortList(s("0")) && !validPortList(s("90-80")));

    for (const char *ok : {"", "lan", "203.0.113.7", "198.51.100.0/24, 2001:db8::/32", "192.0.2.1 - 192.0.2.9"}) CHECK(validRemote(s(ok)));
    for (const char *bad : {"example.com", "0.0.0.0/0", "1.2.3.4 - ::1", "1.2.3.4 } accept"}) CHECK(!validRemote(s(bad)));

    CHECK(validInterface(QString()) && validInterface(s("wlan0")) && validInterface(s("wg*")));
    CHECK(!validInterface(s("lo")) && !validInterface(s("a b")) && !validInterface(s("eth0\"")) && !validInterface(s("0123456789abcdef")));

    CHECK(lookupHost(s(" https://Example.com/x ")) == s("example.com"));
    CHECK(lookupHost(s("8.8.8.0/24")) == s("8.8.8.0/24"));
    CHECK(lookupHost(s("türkiye.example")) == s("xn--trkiye-3ya.example"));
    CHECK(validHost(s("example.com")) && validHost(s("2001:db8::1")));
    CHECK(!validHost(s("-c1")) && !validHost(s("a b")) && !validHost(QString()));

    CHECK(reverseDnsName(s("192.0.2.5")) == s("5.2.0.192.in-addr.arpa"));
    CHECK(reverseDnsName(s("2001:db8::1")).startsWith(s("1.0.0.0.")) && reverseDnsName(s("2001:db8::1")).endsWith(s("8.b.d.0.1.0.0.2.ip6.arpa")));
    CHECK(reverseDnsName(s("nope")).isEmpty());

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("netrules: all checks passed\n");
    return 0;
}
