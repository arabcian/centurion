// Health navigation metadata: search normalization, symptom search, triage links and term explanations.
#include "../src/advanceddiagnostics.h"
#include "../src/toolcatalog.h"

#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    namespace tc = toolcatalog;
    // Turkish letters and case do not matter.
    CHECK(tc::normalize(QStringLiteral("Isınma İşlemci ŞARJ ğüöç")) == QStringLiteral("isinma islemci sarj guoc"));
    CHECK(tc::matches(QStringLiteral("islemci"), QStringLiteral("CPU"), tc::tool(QStringLiteral("CPU")).keywords));
    CHECK(tc::matches(QStringLiteral("  "), QStringLiteral("anything"), {}));
    // Every word must match somewhere.
    CHECK(tc::matches(QStringLiteral("ekran titreme"), QStringLiteral("EDID / VRR"), tc::tool(QStringLiteral("EDID / VRR")).keywords));
    CHECK(!tc::matches(QStringLiteral("ekran pil"), QStringLiteral("EDID / VRR"), tc::tool(QStringLiteral("EDID / VRR")).keywords));
    // Symptoms and codes reach the right place.
    CHECK(tc::matches(QStringLiteral("Xid"), QStringLiteral("Events & alerts"), tc::pageKeywords(QStringLiteral("monitor"))));
    CHECK(tc::matches(QStringLiteral("firmware eksik"), QStringLiteral("x"), tc::tool(QStringLiteral("Wi-Fi / Bluetooth Firmware Doctor")).keywords));
    CHECK(tc::matches(QStringLiteral("vrr"), QStringLiteral("x"), tc::tool(QStringLiteral("EDID / VRR")).keywords));

    // Badges: tools that wake the dGPU or change settings say so.
    CHECK(tc::tool(QStringLiteral("NVIDIA")).flags & tc::MayWakeGpu);
    CHECK(!(tc::tool(QStringLiteral("NVIDIA module parameters")).flags & tc::MayWakeGpu));
    CHECK(tc::tool(QStringLiteral("Configuration files")).flags & tc::ChangesSettings);
    CHECK(tc::badges(tc::MayWakeGpu | tc::NeedsRoot).size() == 2);
    CHECK(tc::tool(QStringLiteral("no such tool")).area == tc::Area::System);

    // Every triage category has somewhere to go next, and none of those links wakes the dGPU.
    for (const advanceddiagnostics::TriageCategory &c : advanceddiagnostics::triageCategories()) {
        const QStringList rel = tc::relatedFor(c.key);
        CHECK(!rel.isEmpty());
        for (const QString &t : rel) {
            CHECK(t.startsWith(QStringLiteral("page:")) || t.startsWith(QStringLiteral("tool:")));
            if (t.startsWith(QStringLiteral("page:"))) CHECK(!tc::pageKeywords(t.mid(5)).isEmpty());
            if (t.startsWith(QStringLiteral("tool:"))) {
                CHECK(!tc::tool(t.mid(5)).keywords.isEmpty());
                CHECK(!(tc::tool(t.mid(5)).flags & tc::MayWakeGpu));
            }
        }
    }

    // Terms: whole words only.
    CHECK(tc::explainTerms(QStringLiteral("ASPM is enabled on 01:00.0")).startsWith(QStringLiteral("ASPM — ")));
    CHECK(tc::explainTerms(QStringLiteral("GSP timeout, D3cold")).split(QLatin1Char('\n')).size() == 2);
    CHECK(tc::explainTerms(QStringLiteral("GASPMX")).isEmpty());
    CHECK(tc::explainTerms(QString()).isEmpty());

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
