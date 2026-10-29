// Unit test for the blocklist subscription rules (blocklistsubs.cpp). Plain asserts, like notifyprefs_test.cpp.
#include "../src/blocklistsubs.h"

#include <QTemporaryDir>

#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    // Parsing: comments, separators, ranges.
    {
        const ParsedBlocklist p = parseBlocklist("# FireHOL style header\n1.2.3.4\n10.0.0.0/8 # private\n\n192.0.2.10 - 192.0.2.80, 2001:db8::/32;203.0.113.7\n");
        CHECK(p.error.isEmpty());
        CHECK(p.entries.size() == 5);
        CHECK(p.entries.value(0) == "1.2.3.4");
        CHECK(p.entries.value(1) == "10.0.0.0/8");
        CHECK(p.entries.value(2) == "192.0.2.10-192.0.2.80");  // a range stays one token
        CHECK(p.entries.value(3) == "2001:db8::/32");
        CHECK(p.entries.value(4) == "203.0.113.7");  // ";" and "," separate tokens too
    }
    CHECK(!parseBlocklist("# only comments\n\n").error.isEmpty());
    CHECK(parseBlocklist("# only comments\n\n").entries.isEmpty());

    // The size limit: exactly the limit passes, one more fails and keeps nothing.
    {
        QByteArray at, over;
        for (int i = 0; i < BLOCKLIST_MAX_ENTRIES; ++i) at += "10.0.0.1\n";
        over = at + "10.0.0.2\n";
        const ParsedBlocklist ok = parseBlocklist(at), big = parseBlocklist(over);
        CHECK(ok.error.isEmpty() && ok.entries.size() == BLOCKLIST_MAX_ENTRIES);
        CHECK(!big.error.isEmpty() && big.entries.isEmpty());
    }

    // URLs: https only, with a host.
    CHECK(blocklistUrlOk("https://iplists.firehol.org/files/firehol_level1.netset"));
    CHECK(!blocklistUrlOk("http://example.org/list.netset"));
    CHECK(!blocklistUrlOk("ftp://example.org/list.netset"));
    CHECK(!blocklistUrlOk("https:///no-host"));
    CHECK(!blocklistUrlOk("not a url"));

    // Due time: never updated is due; then due after the interval, not before.
    {
        BlocklistSub s;
        s.hours = 24;
        CHECK(blocklistDue(s, 1000));
        s.updated = 1000;
        CHECK(!blocklistDue(s, 1000 + 24 * 3600 - 1));
        CHECK(blocklistDue(s, 1000 + 24 * 3600));
    }

    // Storage round trip in a temporary gui.ini; the array is replaced as a whole on save.
    {
        QTemporaryDir dir;
        const QString path = dir.path() + QLatin1String("/gui.ini");
        QList<BlocklistSub> subs(2);
        subs[0].name = QStringLiteral("FireHOL level1");
        subs[0].url = QStringLiteral("https://example.org/a.netset");
        subs[0].hours = 168;
        subs[0].updated = 123456;
        subs[0].entries = 42;
        subs[1].name = QStringLiteral("Second");
        subs[1].url = QStringLiteral("https://example.org/b.netset");
        subs[1].hours = 24;
        subs[1].error = QStringLiteral("HTTP 404");
        saveBlocklistSubs(path, subs);
        QList<BlocklistSub> back = loadBlocklistSubs(path);
        CHECK(back.size() == 2);
        CHECK(back.value(0).name == subs[0].name && back.value(0).url == subs[0].url && back.value(0).hours == 168);
        CHECK(back.value(0).updated == 123456 && back.value(0).entries == 42 && back.value(0).error.isEmpty());
        CHECK(back.value(1).error == "HTTP 404" && back.value(1).hours == 24);
        saveBlocklistSubs(path, {});
        CHECK(loadBlocklistSubs(path).isEmpty());
    }

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("blocklistsubs: all checks passed\n");
    return 0;
}
