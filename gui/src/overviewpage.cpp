#include "overviewpage.h"

#include "advanceddiagnostics.h"
#include "batterypage.h"
#include "dgpupage.h"
#include "reportpreview.h"
#include "sysinfo.h"
#include "telemetryservice.h"
#include "theme.h"
#include "userhelper.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QResizeEvent>
#include <QFont>
#include <algorithm>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <optional>

namespace {

// The levels of overview.rs.
enum { kNoData = -1, kOk = 0, kWatch = 1, kAttention = 2 };
constexpr int DEBOUNCE_MS = 3000;  // a burst of kernel faults is one refresh

QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}

const char *levelColor(int level) {
    if (level == kAttention) return theme::DANGER;
    if (level == kWatch) return theme::WARN;
    if (level == kOk) return theme::OK;
    return theme::MUTED;
}

} // namespace

OverviewPage::OverviewPage(QWidget *parent) : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(6);

    // Prominent health verdict: explicitly describes logged/checked areas only,
    // not a guarantee that every physical component has been tested.
    auto *hero = new QFrame;
    hero->setObjectName(QStringLiteral("healthHero"));
    hero->setStyleSheet(QStringLiteral("QFrame#healthHero{background:%1;border:1px solid %2;border-radius:%3px}"
                                       "QFrame#healthHero QLabel{border:none;background:transparent}")
                        .arg(theme::experimentalEnabled() ? theme::BG1 : theme::BG2, theme::BORDER,
                             theme::experimentalEnabled() ? QStringLiteral("10") : QStringLiteral("13")));
    auto *heroLayout = new QVBoxLayout(hero);
    heroLayout->setContentsMargins(theme::experimentalEnabled() ? 13 : 18,
                                   theme::experimentalEnabled() ? 10 : 15,
                                   theme::experimentalEnabled() ? 13 : 18,
                                   theme::experimentalEnabled() ? 10 : 15);
    heroLayout->setSpacing(7);
    auto *top = new QHBoxLayout;
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    QFont titleFont = summary_->font();
    titleFont.setPointSize(titleFont.pointSize() + 5);
    titleFont.setBold(true);
    summary_->setFont(titleFont);
    top->addWidget(summary_, 1);
    auto *bRefresh = new QPushButton(tr("Check now"));
    bRefresh->setToolTip(tr("Refresh recorded indicators without running a hardware stress test."));
    connect(bRefresh, &QPushButton::clicked, this, &OverviewPage::refresh);
    top->addWidget(bRefresh);
    heroLayout->addLayout(top);
    counts_ = new QLabel;
    counts_->setWordWrap(true);
    counts_->setProperty("tone", "dim");
    heroLayout->addWidget(counts_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setProperty("tone", "muted");
    heroLayout->addWidget(status_);
    sensorStatus_ = new QLabel;
    sensorStatus_->setTextFormat(Qt::PlainText); sensorStatus_->setWordWrap(true); sensorStatus_->setProperty("tone", "muted");
    heroLayout->addWidget(sensorStatus_);
    sensorTimer_ = new QTimer(this);
    sensorTimer_->setInterval(1000);
    connect(sensorTimer_, &QTimer::timeout, this, [this] {
        if (!isVisible()) { sensorStatus_->clear(); return; }
        // Keep only still-fresh evidence visible while the shared flight runs;
        // do not flash unavailable on every timer tick before its reply arrives.
        if (!lastCpuReading_ || !lastCpuReading_->fresh()) {
            lastCpuReading_.reset();
            sensorStatus_->setText(tr("Current CPU temperature: unavailable"));
        }
        if (sensorPending_) return;
        sensorPending_ = true;
        using M = sysinfo::PresentationMetric;
        TelemetryService::instance().request({M::CpuTemp},this,[this](TelemetryService::Results results) {
            sensorPending_ = false;
            if (!isVisible()) return;
            const auto r = results.value(int(M::CpuTemp));
            const auto value = r.values.constFind(QStringLiteral("cpu_temp_c"));
            if (r.fresh() && value != r.values.cend()) {
                lastCpuReading_ = r;
                sensorStatus_->setText(tr("Current CPU temperature: %1 °C").arg(value.value(),0,'f',1));
                const auto left = r.captured + r.ttl - sysinfo::presentationClock();
                // Expire the display independently of the next acquisition.
                QTimer::singleShot(int(std::clamp<qint64>(left.count() + 1, 1, 30000)), this, [this] {
                    if (isVisible() && lastCpuReading_ && !lastCpuReading_->fresh()) {
                        lastCpuReading_.reset();
                        sensorStatus_->setText(tr("Current CPU temperature: unavailable"));
                    }
                });
            } else if (!lastCpuReading_ || !lastCpuReading_->fresh()) {
                lastCpuReading_.reset();
                sensorStatus_->setText(tr("Current CPU temperature: unavailable"));
            }
        });
    });

    outer->addWidget(hero);

    // Start from the symptom: each button runs the read-only guided triage (H18) for that area.
    auto *problems = new QFrame;
    problems->setObjectName(QStringLiteral("healthProblems"));
    problems->setStyleSheet(QStringLiteral("QFrame#healthProblems{background:%1;border:1px solid %2;border-radius:10px}"
                                           "QFrame#healthProblems QLabel{background:transparent;border:none}")
                            .arg(theme::experimentalEnabled() ? theme::BG1 : theme::BG2, theme::BORDER));
    auto *problemsLayout = new QVBoxLayout(problems);
    problemsLayout->setContentsMargins(14, 10, 14, 11);
    problemsLayout->setSpacing(6);
    auto *problemsTitle = new QLabel(tr("Having a problem? Pick what is going wrong:"));
    QFont problemsFont = problemsTitle->font();
    problemsFont.setBold(true);
    problemsTitle->setFont(problemsFont);
    problemsLayout->addWidget(problemsTitle);
    auto *problemsGrid = new QGridLayout;
    problemsGrid->setSpacing(6);
    const QList<advanceddiagnostics::TriageCategory> categories = advanceddiagnostics::triageCategories();
    for (int i = 0; i < categories.size(); ++i) {
        const advanceddiagnostics::TriageCategory &c = categories[i];
        auto *b = new QPushButton(QCoreApplication::translate("advanceddiagnostics", c.shortLabel));
        b->setToolTip(tr("%1\nRuns %2 (read-only) and shows what needs a look, then where to look next.")
                          .arg(QCoreApplication::translate("advanceddiagnostics", c.label), c.checks.join(QStringLiteral(", "))));
        const QString key = c.key;
        connect(b, &QPushButton::clicked, this, [this, key] { Q_EMIT triage(key); });
        problemsGrid->addWidget(b, i / 4, i % 4);
    }
    problemsLayout->addLayout(problemsGrid);
    auto *problemsHint = new QLabel(tr("Nothing is changed and a sleeping dGPU is not woken."));
    problemsHint->setProperty("tone", "muted");
    problemsLayout->addWidget(problemsHint);
    outer->addWidget(problems);

    auto *recommend = new QFrame;
    recommend->setObjectName(QStringLiteral("healthAdvice"));
    recommend->setStyleSheet(QStringLiteral("QFrame#healthAdvice{background:%1;border:1px solid %2;border-radius:%3px}"
                                            "QFrame#healthAdvice QLabel{background:transparent;border:none}")
                             .arg(theme::experimentalEnabled() ? theme::BG1 : theme::BG2, theme::BORDER,
                                  theme::experimentalEnabled() ? QStringLiteral("10") : QStringLiteral("10")));
    auto *adviceLayout = new QHBoxLayout(recommend);
    adviceLayout->setContentsMargins(14, 11, 14, 11);
    recommendation_ = new QLabel;
    recommendation_->setWordWrap(true);
    recommendation_->setTextFormat(Qt::PlainText);
    adviceLayout->addWidget(recommendation_, 1);
    recommendButton_ = new QPushButton(tr("Inspect"));
    connect(recommendButton_, &QPushButton::clicked, this, [this] {
        if (!recommendedTarget_.isEmpty()) Q_EMIT openPage(recommendedTarget_);
    });
    adviceLayout->addWidget(recommendButton_);
    outer->addWidget(recommend);

    auto *section = new QLabel(tr("CHECKED AREAS"));
    section->setStyleSheet(QStringLiteral("color:%1;font-weight:700;letter-spacing:1px").arg(theme::FG_DIM));
    outer->addWidget(section);
    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    auto *cards = new QWidget;
    grid_ = new QGridLayout(cards);
    grid_->setContentsMargins(0, 0, 5, 0);
    grid_->setSpacing(10);
    scroll_->setWidget(cards);
    outer->addWidget(scroll_, 1);

    auto *bottom = new QHBoxLayout;
    auto *hint = new QLabel(tr("Results reflect available logs and measurements, not a complete hardware test. "
                               "Details remain available for troubleshooting."));
    hint->setWordWrap(true);
    hint->setProperty("tone", "muted");
    bottom->addWidget(hint, 1);
    auto *bTools = new QPushButton(tr("Browse System Tools"));
    bTools->setToolTip(tr("Open the searchable tool directory without running any diagnostic."));
    connect(bTools, &QPushButton::clicked, this, [this] { Q_EMIT openPage(QStringLiteral("tools")); });
    bottom->addWidget(bTools);
    auto *bReport = new QPushButton(tr("Diagnostic report…"));
    connect(bReport, &QPushButton::clicked, this, &OverviewPage::report);
    bottom->addWidget(bReport);
    outer->addLayout(bottom);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(DEBOUNCE_MS);
    connect(debounce_, &QTimer::timeout, this, &OverviewPage::refresh);
    summary_->setText(tr("Checking system indicators…"));
    counts_->setText(tr("Results will appear when the first check completes."));
}

void OverviewPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    sensorTimer_->start();
    refresh();
}

void OverviewPage::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    sensorTimer_->stop(); lastCpuReading_.reset(); sensorStatus_->clear();
}

void OverviewPage::scheduleRefresh() { debounce_->start(); }

void OverviewPage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutCards();
}

void OverviewPage::layoutCards() {
    if (!scroll_ || cards_.isEmpty()) return;
    const int columns = scroll_->viewport()->width() >= 650 ? 2 : 1;
    for (QFrame *card : cards_) grid_->removeWidget(card);
    for (int i = 0; i < cards_.size(); ++i)
        grid_->addWidget(cards_.at(i), i / columns, i % columns, Qt::AlignTop);
    grid_->setColumnStretch(0, 1);
    grid_->setColumnStretch(1, columns == 2 ? 1 : 0);
}


QJsonArray OverviewPage::cards() const { return QJsonDocument::fromJson(shown_).array(); }

void OverviewPage::refresh() {
    if (busy_) return;
    busy_ = true;
    QSettings st = guiSettings();
    QJsonArray dirs;
    // The same folders the Backup page uses; the system image is not read from "/".
    const QStringList configured{st.value(QStringLiteral("config_dir"), QDir::homePath() + QStringLiteral("/Backups/centurion")).toString(),
                                 st.value(QStringLiteral("system_dir"), QStringLiteral("/")).toString()};
    for (const QString &d : configured) {
        if (!d.isEmpty() && d != QLatin1String("/")) dirs.append(d);
    }
    // Kernel errors marked seen (Health → Mark all seen) are left out of the kernel card, unclean
    // boots marked reviewed (the card's own button) out of the unclean card.
    const QJsonObject req{{QStringLiteral("op"), QStringLiteral("overview")},
                          {QStringLiteral("battery"), BatteryPage::present()},
                          {QStringLiteral("dgpu"), DgpuPage::present()},
                          {QStringLiteral("backup_dirs"), dirs},
                          {QStringLiteral("kernel_seen"), st.value(QStringLiteral("health/seen_ms"), 0).toLongLong() / 1000},
                          {QStringLiteral("unclean_seen"), st.value(QStringLiteral("health/unclean_seen"), 0).toLongLong()}};
    userhelper::run(req, this, [this](const QJsonObject &r) {
        busy_ = false;
        onReply(r);
    });
}

void OverviewPage::onReply(const QJsonObject &r) {
    if (!r.value(QStringLiteral("ok")).toBool()) {
        status_->setText(tr("Could not read the logs: %1").arg(r.value(QStringLiteral("error")).toString()));
        return;
    }
    const QJsonArray cards = r.value(QStringLiteral("cards")).toArray();
    const QByteArray key = QJsonDocument(cards).toJson(QJsonDocument::Compact);
    if (key != shown_) {
        shown_ = key;
        rebuildCards(cards);
    }
    worst_ = r.value(QStringLiteral("worst")).toInt(kNoData);
    QString text;
    switch (worst_) {
    case kAttention: text = tr("A problem needs attention"); break;
    case kWatch: text = tr("Some items need a closer look"); break;
    case kOk: text = tr("No unreviewed issues in checked areas"); break;
    default: text = tr("More information is needed"); break;
    }
    summary_->setText(text);
    summary_->setStyleSheet(QStringLiteral("color:%1").arg(QString::fromLatin1(levelColor(worst_))));
    int normal = 0, watch = 0, attention = 0, unknown = 0;
    for (const QJsonValue &v : cards) {
        const int level = v.toObject().value(QStringLiteral("level")).toInt(kNoData);
        if (level == kOk) ++normal;
        else if (level == kWatch) ++watch;
        else if (level == kAttention) ++attention;
        else ++unknown;
    }
    counts_->setText(tr("%1 normal  ·  %2 to review  ·  %3 needing attention  ·  %4 not checked")
                         .arg(normal).arg(watch).arg(attention).arg(unknown));
    status_->setText(tr("Last checked %1 · Only logged and available indicators are assessed")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    showRecommendation(cards);
    Q_EMIT worstChanged(worst_);
}

void OverviewPage::showRecommendation(const QJsonArray &cards) {
    QJsonObject selected;
    int highest = kOk;
    for (const QJsonValue &v : cards) {
        const QJsonObject c = v.toObject();
        const int level = c.value(QStringLiteral("level")).toInt(kNoData);
        if (level > highest) { selected = c; highest = level; }
    }
    if (selected.isEmpty()) {
        recommendedTarget_.clear();
        recommendButton_->setVisible(false);
        recommendation_->setText(tr("No urgent action suggested. Review unknown items or create a diagnostic report if you are troubleshooting."));
        return;
    }
    const QString id = selected.value(QStringLiteral("id")).toString();
    recommendedTarget_ = targetFor(id);
    recommendButton_->setVisible(true);
    QString detail;
    if (id == QLatin1String("kernel"))
        detail = tr("A recent kernel or driver error was recorded. Review the events before changing any tuning settings.");
    else if (id == QLatin1String("unclean"))
        detail = tr("One or more sessions ended unexpectedly. Check the boot history for timing and possible causes.");
    else if (id == QLatin1String("battery"))
        detail = tr("Battery capacity or behavior may need attention. Compare the measured values with previous readings.");
    else if (id == QLatin1String("cooling"))
        detail = tr("Review the last cooling test and temperatures. A missing test is not evidence of faulty cooling.");
    else if (id == QLatin1String("disk"))
        detail = tr("A drive reports a SMART warning or wear. Back up important files, then review the drive's values.");
    else if (id == QLatin1String("backup"))
        detail = tr("Check your most recent backup before making further system changes.");
    else if (id == QLatin1String("dgpu"))
        detail = tr("The discrete GPU may be staying awake. Inspect processes keeping it active.");
    else if (id == QLatin1String("sleep"))
        detail = tr("Sleep or wake behavior deserves inspection; check the recorded battery drain and resume events.");
    else detail = tr("Review the recorded details of the highlighted item.");
    recommendation_->setText(tr("Suggested next step: %1").arg(detail));
}

void OverviewPage::rebuildCards(const QJsonArray &cards) {
    while (QLayoutItem *item = grid_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    cards_.clear();
    QList<QJsonObject> sorted;
    for (const QJsonValue &v : cards) sorted.append(v.toObject());
    std::stable_sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("level")).toInt(kNoData) > b.value(QStringLiteral("level")).toInt(kNoData);
    });
    for (const QJsonObject &c : sorted) {
        const QString id = c.value(QStringLiteral("id")).toString();
        const int level = c.value(QStringLiteral("level")).toInt(kNoData);
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("healthCard"));
        card->setStyleSheet(QStringLiteral("QFrame#healthCard{background:%1;border:1px solid %2;border-radius:11px}"
                                           "QFrame#healthCard QLabel{border:none;background:transparent}")
                            .arg(theme::BG2, theme::BORDER));
        auto *col = new QVBoxLayout(card);
        col->setContentsMargins(14, 12, 14, 12);
        col->setSpacing(9);
        auto *head = new QHBoxLayout;
        auto *title = new QLabel(cardTitle(id));
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        head->addWidget(title, 1);
        auto *tag = new QLabel(levelWord(level));
        tag->setStyleSheet(QStringLiteral("color:%1;font-weight:600").arg(QString::fromLatin1(levelColor(level))));
        head->addWidget(tag);
        col->addLayout(head);
        auto *body = new QLabel(cardBody(c));
        body->setWordWrap(true);
        body->setTextFormat(Qt::PlainText);
        body->setProperty("tone", "dim");
        col->addWidget(body);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch(1);
        if (id == QLatin1String("unclean") && level > kOk) {
            const qint64 newest = c.value(QStringLiteral("newest")).toInteger();
            auto *reviewed = new QPushButton(tr("Mark reviewed"));
            reviewed->setToolTip(tr("Acknowledges these shutdowns; does not repair the cause. New shutdowns will appear again."));
            connect(reviewed, &QPushButton::clicked, this, [this, newest] {
                guiSettings().setValue(QStringLiteral("health/unclean_seen"), newest);
                refresh();
            });
            buttons->addWidget(reviewed);
        }
        auto *show = new QPushButton(tr("View details"));
        const QString target = targetFor(id);
        connect(show, &QPushButton::clicked, this, [this, target] { Q_EMIT openPage(target); });
        buttons->addWidget(show);
        col->addLayout(buttons);
        cards_.append(card);
    }
    layoutCards();
}

void OverviewPage::report() {
    if (reporting_) return;
    const auto snapshot = std::make_shared<QTemporaryDir>();
    if (!snapshot->isValid()) {
        QMessageBox::warning(this, tr("Diagnostic Report"), tr("The report could not be built: %1").arg(snapshot->errorString()));
        return;
    }
    reporting_ = true;
    const QString snapshotPath = snapshot->filePath(QStringLiteral("snapshot.json"));
    // Preview first: the same files, with the masked text, are shown before anything is written.
    const QJsonObject preview{{QStringLiteral("op"), QStringLiteral("diag_report")},
                              {QStringLiteral("version"), QStringLiteral(CENTURION_VERSION)},
                              {QStringLiteral("dgpu"), DgpuPage::present()},
                              {QStringLiteral("snapshot"), snapshotPath}};
    userhelper::run(preview, this, [this, preview, snapshot](const QJsonObject &p) {
        if (!p.value(QStringLiteral("ok")).toBool()) {
            reporting_ = false;
            QMessageBox::warning(this, tr("Diagnostic Report"), tr("The report could not be built: %1").arg(p.value(QStringLiteral("error")).toString()));
            return;
        }
        ReportPreview dialog(p, this);
        if (dialog.exec() != QDialog::Accepted) { reporting_ = false; return; }

        QString tag;
        for (const QChar ch : sysinfo::dmiClean(QStringLiteral("product_name")).value_or(QString())) {
            if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_')) tag += ch;
        }
        if (tag.isEmpty()) tag = QStringLiteral("unknown");
        const QString name = QStringLiteral("centurion-report-%1-%2.tar.gz").arg(tag, QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
        QString path = QFileDialog::getSaveFileName(this, tr("Save diagnostic report"), QDir::homePath() + QLatin1Char('/') + name,
                                                    tr("Compressed archives (*.tar.gz)"));
        if (path.isEmpty()) { reporting_ = false; return; }
        if (!path.endsWith(QLatin1String(".tar.gz"))) path += QLatin1String(".tar.gz");

        QJsonObject out{{QStringLiteral("op"), QStringLiteral("diag_report")},
                              {QStringLiteral("version"), QStringLiteral(CENTURION_VERSION)},
                              {QStringLiteral("dgpu"), preview.value(QStringLiteral("dgpu"))},
                              {QStringLiteral("preview_schema"), 2},
                              {QStringLiteral("selected"), dialog.selection()},
                              {QStringLiteral("out"), path}};
        if (p.value(QStringLiteral("frozen")).toBool())
            out.insert(QStringLiteral("snapshot"), preview.value(QStringLiteral("snapshot")));
        userhelper::run(out, this, [this, snapshot](const QJsonObject &w) {
            snapshot->remove();
            reporting_ = false;
            if (!w.value(QStringLiteral("ok")).toBool()) {
                QMessageBox::warning(this, tr("Diagnostic Report"), tr("The archive could not be written: %1").arg(w.value(QStringLiteral("error")).toString()));
                return;
            }
            QMessageBox::information(this, tr("Diagnostic Report"),
                tr("Saved %1 (%2 KiB, %3 files).\n\nSelected files matched the preview. Automatic redaction is best effort; review before sharing.")
                    .arg(w.value(QStringLiteral("file")).toString())
                    .arg((w.value(QStringLiteral("bytes")).toInteger() + 1023) / 1024)
                    .arg(w.value(QStringLiteral("included")).toInt()));
        }, 30000);
    }, 20000);
}

QString OverviewPage::cardTitle(const QString &id) {
    if (id == QLatin1String("kernel")) return tr("Kernel errors");
    if (id == QLatin1String("unclean")) return tr("Unclean shutdowns");
    if (id == QLatin1String("battery")) return tr("Battery Health");
    if (id == QLatin1String("cooling")) return tr("Cooling test");
    if (id == QLatin1String("disk")) return tr("Drive Health");
    if (id == QLatin1String("backup")) return tr("Last backup");
    if (id == QLatin1String("dgpu")) return tr("dGPU awake time today");
    if (id == QLatin1String("sleep")) return tr("Sleep drain");
    return id;
}

QString OverviewPage::cardBody(const QJsonObject &c) {
    const QString id = c.value(QStringLiteral("id")).toString();
    if (c.value(QStringLiteral("level")).toInt(kNoData) == kNoData) {
        // SMART needs the administrator helper, so the Overview never reads it itself: say where the check is.
        if (id == QLatin1String("disk")) return tr("No drive check in the last 30 days. Open the details to run one.");
        return tr("No data yet.");
    }
    if (id == QLatin1String("kernel")) {
        return tr("Last 24 h: %1 critical, %2 errors. Last 7 days: %3 critical, %4 errors.")
            .arg(c.value(QStringLiteral("crit24")).toInt()).arg(c.value(QStringLiteral("err24")).toInt())
            .arg(c.value(QStringLiteral("crit7")).toInt()).arg(c.value(QStringLiteral("err7")).toInt());
    }
    if (id == QLatin1String("unclean")) {
        QString text = tr("%1 of the last %2 boots ended unexpectedly.")
                           .arg(c.value(QStringLiteral("unclean")).toInt()).arg(c.value(QStringLiteral("boots")).toInt());
        if (const int reviewed = c.value(QStringLiteral("reviewed")).toInt()) text += QLatin1Char(' ') + tr("%1 more marked reviewed.").arg(reviewed);
        return text;
    }
    if (id == QLatin1String("battery")) {
        return tr("%1 % of design capacity (%2 Wh full, measured %3).")
            .arg(QString::number(c.value(QStringLiteral("health")).toDouble(), 'f', 0))
            .arg(QString::number(c.value(QStringLiteral("full_wh")).toDouble(), 'f', 1))
            .arg(c.value(QStringLiteral("date")).toString());
    }
    if (id == QLatin1String("cooling")) {
        const QString when = QDateTime::fromSecsSinceEpoch(c.value(QStringLiteral("t")).toInteger()).toString(QStringLiteral("yyyy-MM-dd"));
        const QString verdict = c.value(QStringLiteral("verdict")).toString();
        if (verdict == QLatin1String("ok")) return tr("Last load test (%1): stable.").arg(when);
        if (verdict == QLatin1String("watch")) return tr("Last load test (%1): ran close to the limit.").arg(when);
        if (verdict == QLatin1String("warn")) return tr("Last load test (%1): reached the limit.").arg(when);
        return tr("Last load test (%1): inconclusive.").arg(when);
    }
    if (id == QLatin1String("disk")) {
        // From the recorded SMART samples (Drive health keeps one per day); nothing is read from the drives here.
        const int disks = c.value(QStringLiteral("disks")).toInt();
        const QString date = c.value(QStringLiteral("date")).toString();
        const QString name = c.value(QStringLiteral("name")).toString();
        if (const int faulty = c.value(QStringLiteral("faulty")).toInt())
            return tr("%1 of %2 drive(s) report a SMART warning (%3; checked %4).").arg(faulty).arg(disks).arg(name, date);
        if (const int caution = c.value(QStringLiteral("caution")).toInt())
            return tr("%1 of %2 drive(s) show wear or new media errors (%3; checked %4).").arg(caution).arg(disks).arg(name, date);
        return tr("%1 drive(s), no SMART warnings (checked %2).").arg(disks).arg(date);
    }
    if (id == QLatin1String("backup")) return tr("Last backup %1 day(s) ago.").arg(c.value(QStringLiteral("age_days")).toInt());
    if (id == QLatin1String("dgpu")) {
        return tr("Awake %1 % of the time today (%2 h).")
            .arg(QString::number(c.value(QStringLiteral("awake_pct")).toDouble(), 'f', 0))
            .arg(QString::number(c.value(QStringLiteral("hours")).toDouble(), 'f', 1));
    }
    if (id == QLatin1String("sleep")) {
        // "sleeps" counts every sleep of the week; the drain comes only from those on battery that were long
        // enough to measure. Without one there is no rate: it used to read "draining 0.0 % per hour".
        const QJsonValue perHour = c.value(QStringLiteral("per_hour"));
        if (!perHour.isDouble())
            return tr("%1 sleep(s) in the last 7 days, none long enough on battery to measure the drain; %2 dGPU error(s) on wake.")
                .arg(c.value(QStringLiteral("sleeps")).toInt())
                .arg(c.value(QStringLiteral("dgpu_errors")).toInt());
        return tr("%1 sleep(s) in the last 7 days, draining %2 % per hour on battery; %3 dGPU error(s) on wake.")
            .arg(c.value(QStringLiteral("sleeps")).toInt())
            .arg(QString::number(perHour.toDouble(), 'f', 1))
            .arg(c.value(QStringLiteral("dgpu_errors")).toInt());
    }
    return QString();
}

QString OverviewPage::levelWord(int level) {
    if (level == kAttention) return tr("Needs attention");
    if (level == kWatch) return tr("Review");
    if (level == kOk) return tr("Normal");
    return tr("Not checked");
}

QString OverviewPage::targetFor(const QString &id) {
    if (id == QLatin1String("unclean")) return QStringLiteral("boot");
    if (id == QLatin1String("disk")) return QStringLiteral("smart");
    if (id == QLatin1String("battery") || id == QLatin1String("cooling") || id == QLatin1String("backup") ||
        id == QLatin1String("dgpu") || id == QLatin1String("sleep")) return id;
    return QStringLiteral("monitor");
}
