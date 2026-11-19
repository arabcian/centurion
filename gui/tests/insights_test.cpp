// Offline, hardware-independent regression tests for the six-feature analysis views.
#include "../src/insights.h"
#include <QApplication>
#include <QJsonDocument>
#include <QByteArray>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
static QString joined(const QStringList &lines) { return lines.join(QLatin1Char('\n')); }
static QJsonArray array(const char *literal) { return QJsonDocument::fromJson(literal).array(); }

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto report = QJsonDocument::fromJson(R"({"decisions":[
      {"key":"measured.item","source":"measured","seen":true,"n":12,"in_context":[0.3,0.02]},
      {"key":"unseen.item","source":"estimate","seen":false,"n":0,"in_context":[0.1,0.4]}
    ]})").object();
    const QString confidence = joined(insights::confidence(report));
    CHECK(confidence.contains("CALIBRATION CONFIDENCE EXPLORER"));
    CHECK(confidence.contains("unseen.item"));
    CHECK(confidence.indexOf("unseen.item") < confidence.indexOf("measured.item"));
    CHECK(joined(insights::confidence({})).contains("No structured"));

    const auto runs = array(R"([
      {"t":100,"key":"same","secs":120,"noisy":false,"start_temp":45,"fan_rpm":1900,"steady_temp":80,"mhz":4500},
      {"t":200,"key":"same","secs":120,"noisy":false,"start_temp":46,"fan_rpm":2000,"steady_temp":82,"mhz":4400},
      {"t":300,"key":"other","secs":120,"noisy":false,"start_temp":45,"fan_rpm":1200,"steady_temp":65,"mhz":5100}
    ])");
    const QString pareto = joined(insights::pareto(runs));
    CHECK(pareto.contains("PARETO FRONTIER"));
    CHECK(pareto.contains("dominated"));
    CHECK(pareto.contains("2 matching 120-second runs"));
    CHECK(!pareto.contains("3 matching 120-second runs"));
    const auto incompatible = array(R"([
      {"t":100,"key":"same","secs":120,"noisy":false,"start_temp":40,"fan_rpm":1900,"steady_temp":80,"mhz":4500},
      {"t":200,"key":"same","secs":120,"noisy":false,"start_temp":50,"fan_rpm":2000,"steady_temp":82,"mhz":4400}
    ])");
    CHECK(joined(insights::pareto(incompatible)).contains("start temperatures differ"));
    CHECK(joined(insights::pareto({})).contains("Need at least two"));
    const QString cap = joined(insights::capabilities());
    CHECK(cap.contains("CAPABILITY DOCTOR"));
    CHECK(cap.contains("READ ONLY"));
    const QString inc = joined(insights::incident(QJsonObject{{"kind", "xid"}, {"title", "GPU fault"}}, QString()));
    CHECK(inc.contains("Boot ID absent"));
    CHECK(inc.contains("Time proximity"));
    if (failures) std::fprintf(stderr, "%d regression failure(s)\n", failures);
    else std::puts("insights: ok");
    return failures ? 1 : 0;
}
