#include "healthevidence.h"

#include "advancedhealth.h"
#include "diagreportview.h"
#include "diagsources.h"
#include "evidencereports.h"
#include "incidentfocus.h"
#include "privileged.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

static QString samplePath() { return diagsrc::dataDir() + QStringLiteral("/aer-last-sample.json"); }

HealthEvidencePage::HealthEvidencePage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 7, 8, 7);
    // One row of controls: Scan, then the incident focus that filters all five reports. The description is
    // the status line until the first scan, then that scan's summary.
    auto *focusRow = new QHBoxLayout;
    scan_ = new QPushButton(tr("Scan"));
    scan_->setToolTip(tr("One read-only scan of this boot's kernel log and device counters, read five ways. Nothing runs in the background."));
    focusRow->addWidget(scan_);
    focusRow->addSpacing(8);
    focusRow->addWidget(new QLabel(tr("Incident focus:")));
    focus_ = new QComboBox;
    focus_->setEditable(true);
    focus_->setMinimumContentsLength(18);
    focus_->setToolTip(tr("A CPU, PCI address, Xid code or any text. Every report below is filtered by it, so the same incident is "
                          "read in each tool. Suggestions come from the last scan."));
    focus_->lineEdit()->setPlaceholderText(tr("e.g. CPU 5, 01:00.0, Xid 79"));
    focusRow->addWidget(focus_, 1);
    auto *clearFocus = new QPushButton(tr("Clear focus"));
    focusRow->addWidget(clearFocus);
    layout->addLayout(focusRow);
    status_ = new QLabel(tr("One read-only scan of this boot's kernel log and device counters, read five ways. Press Scan; nothing runs in the background."));
    status_->setWordWrap(true);
    status_->setProperty("role", "muted");
    layout->addWidget(status_);
    tabs_ = new QTabWidget(this);
    const QStringList names = {tr("H02 · Machine Checks"), tr("H03 · PCIe Errors"), tr("H04 · NVIDIA Xid"), tr("H05 · F2FS"), tr("H09 · Wi-Fi / Bluetooth")};
    for (int i = 0; i < 5; ++i) {
        views_[i] = new DiagReportView;
        views_[i]->setCompact(true);
        views_[i]->setMessage(tr("Press Scan to read this boot's evidence."));
        tabs_->addTab(views_[i], names.at(i));
    }
    layout->addWidget(tabs_, 1);
    connect(scan_, &QPushButton::clicked, this, [this] { collect(); });
    connect(focus_, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        applyFocus();
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"))
            .setValue(QStringLiteral("health/evidenceFocus"), t.trimmed());
    });
    connect(clearFocus, &QPushButton::clicked, this, [this] { focus_->setEditText(QString()); });
    focus_->setEditText(QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("centurion"), QStringLiteral("gui"))
                            .value(QStringLiteral("health/evidenceFocus")).toString());
}

void HealthEvidencePage::applyFocus() {
    const QString filter = incidentfocus::filterFor(focus_->currentText());
    for (auto *v : views_) v->setFilterText(filter);
    status_->setToolTip(filter.isEmpty() ? QString() : tr("Reports filtered by the incident focus \"%1\".").arg(filter));
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
    // Suggestions for the focus from what this scan reported; the current focus is kept and re-applied.
    QString all;
    for (auto *v : views_) all += v->report().toText() + QLatin1Char('\n');
    {
        const QString keep = focus_->currentText();
        const QSignalBlocker block(focus_);
        focus_->clear();
        focus_->addItems(incidentfocus::candidates(all));
        focus_->setEditText(keep);
    }
    applyFocus();
    for (int i = 0; i < 5; ++i) {
        const diag::Level l = views_[i]->report().verdict();
        tabs_->setTabText(i, tabs_->tabText(i).section(QStringLiteral(" ●"), 0, 0) + (l >= diag::Level::Review ? QStringLiteral(" ●") : QString()));
    }
}
