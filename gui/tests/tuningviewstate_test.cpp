#include "../src/tuningviewstate.h"
#include <iostream>
using tuningview::BootMatch;
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char *name) { if (!ok) { std::cerr << name << '\n'; ++failures; } };
    check(tuningview::bootMatch(false, true, "0", "0") == BootMatch::NotSaved, "not saved");
    check(tuningview::bootMatch(true, false, "0", "0") == BootMatch::Unknown, "stale or cached");
    check(tuningview::bootMatch(true, true, "", "0") == BootMatch::Unknown, "unreadable");
    check(tuningview::bootMatch(true, true, "0", "") == BootMatch::Unknown, "invalid saved value");
    check(tuningview::bootMatch(true, true, "0", "0") == BootMatch::Matches, "zero is valid");
    check(tuningview::bootMatch(true, true, "0", "1") == BootMatch::Different, "mismatch");
    check(tuningview::valueText(QJsonValue(qint64(8589934592LL))) == "8589934592", "64-bit value");
    check(tuningview::valueText(QJsonValue()) .isEmpty(), "missing value");
    check(tuningview::valueText(QJsonValue(QStringLiteral("performance"))) == "performance", "string");
    return failures ? 1 : 0;
}
