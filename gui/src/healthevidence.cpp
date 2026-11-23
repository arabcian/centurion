#include "healthevidence.h"

#include "advancedhealth.h"
#include "diagreportview.h"
#include "diagsources.h"
#include "evidencereports.h"
#include "privileged.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

static QString samplePath() { return diagsrc::dataDir() + QStringLiteral("/aer-last-sample.json"); }

HealthEvidencePage::HealthEvidencePage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 7, 8, 7);
    auto *top = new QHBoxLayout;
    auto *info = new QLabel(tr("One read-only scan of this boot's kernel log and device counters, read five ways. Press Scan; nothing runs in the background."));
    info->setWordWrap(true);
    info->setProperty("role", "muted");
    top->addWidget(info, 1);
    scan_ = new QPushButton(tr("Scan"));
    top->addWidget(scan_);
    layout->addLayout(top);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setProperty("role", "muted");
    layout->addWidget(status_);
    tabs_ = new QTabWidget(this);
    const QStringList names = {tr("H02 · Machine Checks"), tr("H03 · PCIe Errors"), tr("H04 · NVIDIA Xid"), tr("H05 · F2FS"), tr("H09 · Wi-Fi / Bluetooth")};
    for (int i = 0; i < 5; ++i) {
        views_[i] = new DiagReportView;
        views_[i]->setMessage(tr("Press Scan to read this boot's evidence."));
        tabs_->addTab(views_[i], names.at(i));
    }
    layout->addWidget(tabs_, 1);
    connect(scan_, &QPushButton::clicked, this, [this] { collect(); });
}

void HealthEvidencePage::collect() {
    if (busy_) return;
    busy_ = true;
    scan_->setEnabled(false);
    status_->setText(tr("Scanning the kernel log and device counters (read-only)…"));
    investigations::fetch(this, {QStringLiteral("diagnostics")}, [this](const QHash<QString, QJsonObject> &data, const QStringList &failed) {
        busy_ = false;
        scan_->setEnabled(true);
        if (!data.contains(QStringLiteral("diagnostics"))) {
            // Earlier results stay; an unreadable log is never shown as a clean one.
            status_->setText(tr("Scan not possible: %1").arg(failed.join(QStringLiteral("; "))));
            return;
        }
        display(data.value(QStringLiteral("diagnostics")));
    });
}

void HealthEvidencePage::display(const QJsonObject &r) {
    QHash<QString, QJsonObject> data{{QStringLiteral("diagnostics"), r}};
    views_[0]->setReport(investigations::reportFor(QStringLiteral("H02"), data));
    views_[1]->setReport(investigations::reportFor(QStringLiteral("H03"), data));
    views_[2]->setReport(investigations::reportFor(QStringLiteral("H04"), data));
    views_[3]->setReport(investigations::reportFor(QStringLiteral("H05"), data));
    views_[4]->setReport(investigations::reportFor(QStringLiteral("H09"), data));
    // This scan's AER counters are the next scan's comparison point.
    const QString err = r.value(QStringLiteral("boot_id")).toString().isEmpty() ? QString() : diagsrc::saveJson(samplePath(), evidencereports::aerSample(r));
    status_->setText(tr("Boot %1 · %2 classified event(s) read (the newest 300 at most).%3")
                         .arg(r.value(QStringLiteral("boot_id")).toString().left(8)).arg(r.value(QStringLiteral("event_count")).toInt())
                         .arg(err.isEmpty() ? QString() : tr(" AER sample not saved: %1").arg(err)));
    for (int i = 0; i < 5; ++i) {
        const diag::Level l = views_[i]->report().verdict();
        tabs_->setTabText(i, tabs_->tabText(i).section(QStringLiteral(" ●"), 0, 0) + (l >= diag::Level::Review ? QStringLiteral(" ●") : QString()));
    }
}
