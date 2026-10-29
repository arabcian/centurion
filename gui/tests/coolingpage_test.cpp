// Cooling page: the verdict wording, and the page against a fake `centurion-cooltest` on PATH
// (status table, progress while running, result and error handling).
//   cmake -DCENTURION_BUILD_TESTS=ON .. && make centurion-cooling-test && ctest
// Plain asserts (no QtTest dependency); runs on the offscreen platform.
#include "../src/coolingpage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QJsonObject obj(const char *json) { return QJsonDocument::fromJson(json).object(); }

template <class F> static bool waitFor(F cond, int ms = 8000) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

static QLabel *banner(CoolingPage &p) {
    for (QLabel *l : p.findChildren<QLabel *>()) if (l->objectName() == QLatin1String("coolBanner")) return l;
    return nullptr;
}

static const char *STATUS_WARN = R"({"ok":true,"has_sensor":true,"fans":1,"limit_c":85.0,
 "context":{"profile":"custom","power":"ac","limits":{"ppt_pl1_spl":80,"ppt_pl2_sppt":110,"ppt_pl3_fppt":120,"cpu_temp":85},"threads":32},
 "baseline":{"t":1700000000,"steady_temp":84.5,"fan_rpm":4800.0,"mhz":4300.0,"start_temp":45.0,"noisy":false,"baseline":true},
 "runs":[{"t":1700000000,"steady_temp":84.5,"fan_rpm":4800.0,"mhz":4300.0,"start_temp":45.0,"noisy":false,"baseline":true},
         {"t":1710000000,"steady_temp":84.8,"fan_rpm":5200.0,"mhz":3800.0,"start_temp":46.0,"noisy":false,"baseline":false}],
 "assessment":{"level":"warn","d_temp":0.3,"d_clock_pct":11.6,"d_fan_pct":8.3,"pinned":true,
   "reasons":[{"code":"pinned","value":85.0},{"code":"clock_down","value":11.6}]}})";

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // ── verdict wording ────────────────────────────────────────────────────────
    {
        const QString w = CoolingPage::verdictText(obj(STATUS_WARN)["assessment"].toObject(), 85);
        CHECK(w.contains(QStringLiteral("clearly weaker")));
        CHECK(w.contains(QStringLiteral("clock 12 % lower")));          // 11.6 rounded
        CHECK(w.contains(QStringLiteral("temperature target")) && w.contains(QStringLiteral("85")));
        CHECK(!w.contains(QStringLiteral("fan curve")));                 // no fan figure, no fan caveat
        const QString noisy = CoolingPage::verdictText(obj(R"({"level":"inconclusive","reasons":[{"code":"noisy","value":0}]})"), 95);
        CHECK(noisy.contains(QStringLiteral("Other programs")));
        const QString warm = CoolingPage::verdictText(obj(R"({"level":"inconclusive","reasons":[{"code":"warm_start","value":7.2}]})"), 95);
        CHECK(warm.contains(QStringLiteral("7 °C warmer")));
        const QString ok = CoolingPage::verdictText(obj(R"({"level":"ok","d_temp":0.4,"pinned":false,"reasons":[]})"), 95);
        CHECK(ok.contains(QStringLiteral("as good as the baseline")));
        const QString better = CoolingPage::verdictText(obj(R"({"level":"better","d_temp":-9.0,"pinned":false,"reasons":[{"code":"better","value":-9.0}]})"), 95);
        CHECK(better.contains(QStringLiteral("-9.0")) && better.contains(QStringLiteral("baseline")));
        const QString fan = CoolingPage::verdictText(obj(R"({"level":"watch","d_temp":4.5,"pinned":false,"reasons":[{"code":"temp_up","value":4.5},{"code":"fan_up","value":16}]})"), 95);
        CHECK(fan.contains(QStringLiteral("4.5 °C hotter")) && fan.contains(QStringLiteral("fan curve")));
        const QString split = CoolingPage::verdictText(obj(R"({"level":"watch","d_temp":0.5,"pinned":false,"reasons":[{"code":"fan_split","value":22}]})"), 95);
        CHECK(split.contains(QStringLiteral("22 points further apart")));

        // Fan max test: the peaks against their baseline.
        bool warn = false;
        const QString peaks = CoolingPage::fanMaxText(obj(R"({"ok":true,"baseline":false,"peaks":[3600,3900],"base_peaks":[4000,4000],"drops_pct":[10.0,2.5],"warn":true,"warn_pct":8.0})"), &warn);
        CHECK(warn);
        CHECK(peaks.contains(QStringLiteral("3600 rpm")) && peaks.contains(QStringLiteral("fan 1 peaks 10 % under")) && !peaks.contains(QStringLiteral("fan 2 peaks")));
        const QString calm = CoolingPage::fanMaxText(obj(R"({"ok":true,"baseline":false,"peaks":[3900],"drops_pct":[2.5],"warn":false,"warn_pct":8.0})"), &warn);
        CHECK(!warn && calm.contains(QStringLiteral("No fan peaks more than 8 %")));
        const QString first = CoolingPage::fanMaxText(obj(R"({"ok":true,"baseline":true,"peaks":[4000,3900]})"), &warn);
        CHECK(!warn && first.contains(QStringLiteral("baseline")));
        CHECK(CoolingPage::fanMaxText(obj(R"({"ok":false,"error":"no fans"})"), &warn).contains(QStringLiteral("no fans")));
    }

    // ── the page against a fake tool ───────────────────────────────────────────
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString script = dir.filePath(QStringLiteral("centurion-cooltest"));
    {
        QFile f(script);
        CHECK(f.open(QIODevice::WriteOnly));
        QByteArray s = "#!/bin/sh\ncase \"$1\" in\n status) cat <<'EOF'\n" + QByteArray(STATUS_WARN).simplified() + "\nEOF\n ;;\n"
            " run) echo '{\"phase\":\"cooldown\",\"total\":120}'\n"
            "      echo '{\"phase\":\"load\",\"elapsed\":60.0,\"total\":240,\"temp\":81.0,\"fan\":4100.0,\"mhz\":4100.0}'\n"
            "      sleep 1\n"
            "      echo '{\"ok\":false,\"error\":\"stopped for safety: test\"}' ; exit 1 ;;\n"
            " forget|baseline) echo '{\"ok\":true}' ;;\nesac\n";
        f.write(s);
        f.setPermissions(f.permissions() | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
    }
    qputenv("PATH", (dir.path() + QLatin1Char(':') + QString::fromLocal8Bit(qgetenv("PATH"))).toLocal8Bit());

    CoolingPage page;
    page.show();
    auto *table = page.findChild<QTreeWidget *>();
    CHECK(table != nullptr);
    CHECK(waitFor([&] { return table->topLevelItemCount() == 2; }));
    if (table->topLevelItemCount() == 2) {
        // Newest first; the baseline row says so, the other is compared with it.
        CHECK(table->topLevelItem(0)->text(1) == QStringLiteral("84.8 °C"));
        CHECK(table->topLevelItem(0)->text(5) == QStringLiteral("+0.3 °C"));
        CHECK(table->topLevelItem(1)->text(5) == QStringLiteral("baseline"));
        CHECK(table->topLevelItem(0)->text(3) == QStringLiteral("3.80 GHz"));
    }
    QLabel *b = banner(page);
    CHECK(b && b->isVisible() && b->text().contains(QStringLiteral("clearly weaker")));
    QPushButton *run = nullptr, *cancel = nullptr;
    for (QPushButton *p : page.findChildren<QPushButton *>()) {
        if (p->text() == QStringLiteral("Run cooling test")) run = p;
        if (p->text() == QStringLiteral("Cancel")) cancel = p;
    }
    CHECK(run && cancel && run->isEnabled() && !cancel->isVisible());

    // A run that fails: progress is shown while it goes, the reason afterwards, and the page is usable again.
    if (run) {
        run->click();
        auto *bar = page.findChild<QProgressBar *>();
        CHECK(waitFor([&] { return bar && bar->isVisible() && bar->maximum() == 240 && bar->value() == 60; }));
        CHECK(cancel->isVisible() && !run->isEnabled());
        CHECK(waitFor([&] { return run->isEnabled(); }));
        CHECK(!cancel->isVisible());
        CHECK(waitFor([&] { QLabel *x = banner(page); return x && x->text().contains(QStringLiteral("stopped for safety")); }));
    }
    return failures ? 1 : 0;
}
