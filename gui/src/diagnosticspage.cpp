#include "diagnosticspage.h"

#include "advancedhealth.h"
#include "bootpage.h"
#include "healthevidence.h"
#include "lazywidget.h"
#include "theme.h"

#include <QTabWidget>
#include <QVBoxLayout>

DiagnosticsPage::DiagnosticsPage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget;
    if (theme::experimentalEnabled()) tabs_->setDocumentMode(true);
    boots_ = new BootPage;
    findings_ = new AdvancedHealthPage;
    tabs_->addTab(boots_, tr("Boot History"));
    // Built when first opened (a few hundred widgets, no background duty): see HealthTab.
    tabs_->addTab(new LazyWidget([] { return new HealthEvidencePage; }), tr("This Boot"));
    tabs_->addTab(findings_, tr("Findings"));
    tabs_->setTabToolTip(Boots, tr("Every boot: how it ended, its kernel events and faults, the curves in effect, crash records and patterns."));
    tabs_->setTabToolTip(ThisBoot, tr("The running boot's own evidence, per component: machine checks, kernel, memory, PCIe, storage, file systems, GPUs, thermal and power, USB, audio, radios, firmware and drivers."));
    tabs_->setTabToolTip(Findings, tr("Checks that compare boots and versions (did an update make things worse?), plus NVMe, USB, sleep and GPU memory checks."));
    layout->addWidget(tabs_);
    // "Review" on a finding about boots: those boots, highlighted, in Boot History.
    connect(findings_, &AdvancedHealthPage::showBoots, this, [this](const QStringList &ids, const QString &why) {
        showTab(Boots);
        boots_->highlightBoots(ids, why);
    });
}

void DiagnosticsPage::showTab(Tab tab) { tabs_->setCurrentIndex(tab); }

int DiagnosticsPage::tabForKey(const QString &key) {
    if (key == QLatin1String("boot")) return Boots;
    if (key == QLatin1String("evidence")) return ThisBoot;
    if (key == QLatin1String("investigations")) return Findings;
    return -1;
}

QString DiagnosticsPage::tabTitle(int tab) const { return tabs_->tabText(tab); }
