#pragma once
// Health → Problems → Diagnostics: what went wrong, in one place with three tabs instead of three pages.
//   Boot History — every boot, how it ended, its kernel events and faults (BootPage)
//   This Boot    — the running boot's own evidence: machine checks, PCIe, Xid, F2FS, radios (HealthEvidencePage)
//   Findings     — checks that compare boots and versions, plus the occasional ones (AdvancedHealthPage)
// A finding about boots (Findings → "Review") opens Boot History with exactly those boots highlighted.
#include <QString>
#include <QWidget>

class AdvancedHealthPage;
class BootPage;
class HealthEvidencePage;
class QTabWidget;

class DiagnosticsPage : public QWidget {
    Q_OBJECT
public:
    enum Tab { Boots = 0, ThisBoot = 1, Findings = 2 };
    explicit DiagnosticsPage(QWidget *parent = nullptr);
    BootPage *boots() const { return boots_; }
    AdvancedHealthPage *findings() const { return findings_; }
    void showTab(Tab tab);
    /// The tab an earlier page key ("boot", "evidence", "investigations") stands for; -1 for any other key.
    static int tabForKey(const QString &key);
    QString tabTitle(int tab) const;

private:
    QTabWidget *tabs_;
    BootPage *boots_;
    HealthEvidencePage *evidence_;
    AdvancedHealthPage *findings_;
};
