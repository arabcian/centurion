#include "simulation_shell.h"
#include "simulationguard.h"
#include "mainwindow.h"
#include "healthtab.h"
#include "theme.h"
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QPixmap>
#include <QEventLoop>
#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <algorithm>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <cstdio>

namespace simulation {
int runRealShell(QApplication &app) {
    if (!simulationguard::active.load()) return 2;
    app.setFont(QFont(QStringLiteral("DejaVu Sans"), 9));
    QSettings privateSettings(QSettings::IniFormat,QSettings::UserScope,QStringLiteral("centurion"),QStringLiteral("gui"));
    privateSettings.setValue(QStringLiteral("first_run/done"),true);
    privateSettings.sync();
    if (privateSettings.status()!=QSettings::NoError || !theme::saveTheme(qEnvironmentVariable("CENTURION_FIXTURE_THEME"))) return 3;
    theme::load();
    theme::applyBase(app);
    if (qEnvironmentVariable("CENTURION_LANG")==QStringLiteral("tr") && !QFile::exists(QStringLiteral(":/i18n/centurion_tr.qm"))) return 3;
    MainWindow window;
    theme::applySheet(app);
    window.setWindowTitle(QStringLiteral("Centurion — SIMULATION / FIXTURE ONLY"));
    window.setHideOnClose(false);
    const auto size=qEnvironmentVariable("CENTURION_FIXTURE_SIZE").split(QLatin1Char('x'));
    if (size.size()!=2) return 3;
    window.resize(size[0].toInt(),size[1].toInt());
    window.show();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabs"));
    if (!tabs) return 3;
    QTimer::singleShot(300, &window, [&] {
        QJsonArray pages, shots;
        bool ok = window.size()==QSize(size[0].toInt(),size[1].toInt()) &&
            std::abs(window.devicePixelRatioF()-qEnvironmentVariable("QT_SCALE_FACTOR").toDouble())<0.02;
        const auto expected = qEnvironmentVariable("CENTURION_FIXTURE_PAGES").split(QLatin1Char(','), Qt::SkipEmptyParts);
        QStringList actual;
        for (int i=0; i<tabs->count(); ++i) actual << tabs->tabBar()->tabData(i).toString();
        auto sortedExpected=expected; sortedExpected.sort(); actual.sort();
        if (actual!=sortedExpected || tabs->count()>32) ok=false;
        // Select real pages, not a synthetic widget tree. Nested tabs are
        // exercised when visible; modal dialogs or hangs fail the launcher timeout.
        auto capture = [&](const QString &name) {
            // Let queued fixture failures and lazy layout work reach the event loop.
            QEventLoop settle;
            QTimer::singleShot(40,&settle,&QEventLoop::quit);
            settle.exec();
            const QImage image=window.grab().toImage();
            if (image.isNull() || !image.save(QStringLiteral("/output/")+name)) ok=false;
            else shots.append(name);
        };
        for (int i=0; i<tabs->count() && i<32; ++i) {
            if (!tabs->isTabEnabled(i)) continue;
            tabs->setCurrentIndex(i); app.processEvents();
            pages.append(tabs->tabBar()->tabData(i).toString());
            capture(QStringLiteral("page-%1.png").arg(i,2,10,QLatin1Char('0')));
            QWidget *page=tabs->currentWidget();
            if (!page) { ok=false; continue; }
            QList<QPointer<QTabWidget>> nested;
            for (auto *t : page->findChildren<QTabWidget *>()) if (t->isVisible()) nested.append(QPointer<QTabWidget>(t));
            for (int j=0; j<nested.size() && j<12; ++j) {
                auto t=nested[j];
                if (!t || !t->isVisible()) continue;
                for (int k=0; t && k<t->count() && k<24; ++k) {
                    if (!t->isTabEnabled(k)) continue;
                    t->setCurrentIndex(k); app.processEvents();
                    if (shots.size()>=160) { ok=false; break; }
                    capture(QStringLiteral("page-%1-sub-%2-%3.png").arg(i,2,10,QLatin1Char('0')).arg(j).arg(k));
                }
            }
        }
        QJsonArray healthTargets;
        if (auto *health=window.findChild<HealthTab *>()) {
            window.showTab(QStringLiteral("Health")); app.processEvents();
            auto entries=health->navigationEntries();
            std::sort(entries.begin(),entries.end(),[](const auto &a,const auto &b){return a.target<b.target;});
            if (entries.size()>96) ok=false;
            for (int i=0;i<entries.size() && i<96;++i) {
                if (!health->openNavigationTarget(entries[i].target)) {ok=false;continue;}
                app.processEvents(); healthTargets.append(entries[i].target);
                if (shots.size()>=256) {ok=false;break;}
                capture(QStringLiteral("health-%1.png").arg(i,3,10,QLatin1Char('0')));
            }
        } else ok=false;
        if (window.size()!=QSize(size[0].toInt(),size[1].toInt())) ok=false;
        QJsonObject manifest{{"schema",1},{"fixture",qEnvironmentVariable("CENTURION_FIXTURE_ID")},
            {"mode",theme::experimentalEnabled()?QStringLiteral("experimental"):QStringLiteral("classic")},
            {"theme",theme::currentTheme()},{"scale",qEnvironmentVariable("QT_SCALE_FACTOR")},
            {"language",qEnvironmentVariable("CENTURION_LANG")},{"requested_size",qEnvironmentVariable("CENTURION_FIXTURE_SIZE")},
            {"actual_width",window.width()},{"actual_height",window.height()},{"actual_dpr",window.devicePixelRatioF()},
            {"pages",pages},{"health_targets",healthTargets},{"screenshots",shots},{"blocked_root_requests",int(simulationguard::blockedRoot.load())},
            {"blocked_user_requests",int(simulationguard::blockedUser.load())},{"ok",ok}};
        QSaveFile report(QStringLiteral("/output/manifest.json"));
        const QByteArray bytes=QJsonDocument(manifest).toJson();
        if (!report.open(QIODevice::WriteOnly) || report.write(bytes)!=bytes.size() || !report.commit()) ok=false;
        app.exit(ok?0:4);
    });
    return app.exec();
}
} // namespace simulation
