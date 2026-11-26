#include "configpage.h"

#include "sysadmin.h"
#include "theme.h"

#include <QColor>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iterator>
#include <vector>

// Must match CONFS in crates/centurion-helpers/src/sysadmin.rs.
enum class Kind { Sysctl, Modprobe, ModulesLoad, Udev, Fstab, ShellVars };

struct ConfigPage::File {
    const char *id, *path, *label, *note;
    Kind kind;
};

namespace {

const ConfigPage::File *files();
int fileCount();

QString backupDir(const char *id) { return QStringLiteral("/var/lib/centurion/config-backups/") + QLatin1String(id); }

QString readText(const QString &path, bool *ok = nullptr) {
    QFile f(path);
    const bool open = f.open(QIODevice::ReadOnly);
    if (ok) *ok = open || !f.exists();
    return open ? QString::fromUtf8(f.read(256 * 1024)) : QString();
}

bool structured(Kind k) { return k == Kind::Sysctl || k == Kind::Modprobe || k == Kind::ModulesLoad; }

/// One entry line split into table cells; empty when the line is a comment or not an entry.
QStringList parse(Kind k, const QString &raw) {
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';'))) return {};
    switch (k) {
    case Kind::Sysctl: {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) return {};
        return {line.left(eq).trimmed(), line.mid(eq + 1).trimmed()};
    }
    case Kind::Modprobe: {
        const QStringList w = line.split(QRegularExpression(QStringLiteral("\\s+")));
        if (w.size() < 2 || (w[0] != QLatin1String("options") && w[0] != QLatin1String("blacklist"))) return {};
        return {w[0], w[1], w.mid(2).join(QLatin1Char(' '))};
    }
    case Kind::ModulesLoad:
        return line.contains(QLatin1Char(' ')) ? QStringList{} : QStringList{line};
    default:
        return {};
    }
}

QString format(Kind k, const QStringList &c) {
    switch (k) {
    case Kind::Sysctl: return QStringLiteral("%1 = %2").arg(c.value(0).trimmed(), c.value(1).trimmed());
    case Kind::Modprobe: return QStringList{c.value(0).trimmed(), c.value(1).trimmed(), c.value(2).trimmed()}.join(QLatin1Char(' ')).trimmed();
    default: return c.value(0).trimmed();
    }
}

QStringList headers(Kind k) {
    switch (k) {
    case Kind::Sysctl: return {ConfigPage::tr("Key"), ConfigPage::tr("Value"), ConfigPage::tr("Now")};
    case Kind::Modprobe: return {ConfigPage::tr("Command"), ConfigPage::tr("Module"), ConfigPage::tr("Parameters")};
    default: return {ConfigPage::tr("Module"), ConfigPage::tr("Loaded now")};
    }
}

QString sysctlNow(const QString &key) {
    QString rel = key;
    if (rel.startsWith(QLatin1Char('-'))) rel.remove(0, 1);
    if (!rel.contains(QLatin1Char('/'))) rel.replace(QLatin1Char('.'), QLatin1Char('/'));
    QFile f(QStringLiteral("/proc/sys/") + rel);
    if (rel.contains(QLatin1String("..")) || rel.contains(QLatin1Char('*')) || !f.open(QIODevice::ReadOnly)) return QStringLiteral("—");
    return QString::fromUtf8(f.read(256)).simplified();
}

QStringList sysctlKeys() {
    static QStringList keys;
    if (!keys.isEmpty()) return keys;
    QDirIterator it(QStringLiteral("/proc/sys"), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) keys << it.next().mid(10).replace(QLatin1Char('/'), QLatin1Char('.'));
    keys.sort();
    return keys;
}

QStringList moduleNames() {
    return QDir(QStringLiteral("/sys/module")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

const ConfigPage::File FILES[] = {
    {"sysctl", "/etc/sysctl.d/99-centurion-user.conf", QT_TRANSLATE_NOOP("ConfigPage", "Kernel settings (sysctl, Centurion's file)"),
     QT_TRANSLATE_NOOP("ConfigPage", "Applied at once (sysctl -p) and at every boot. Emptying the file removes it; values already applied stay until reboot."), Kind::Sysctl},
    {"modprobe", "/etc/modprobe.d/centurion-user.conf", QT_TRANSLATE_NOOP("ConfigPage", "Module options and blacklist (modprobe.d)"),
     QT_TRANSLATE_NOOP("ConfigPage", "Takes effect the next time the module loads; for drivers in the initramfs, regenerate it (dracut) and reboot."), Kind::Modprobe},
    {"modules_load", "/etc/modules-load.d/centurion-user.conf", QT_TRANSLATE_NOOP("ConfigPage", "Modules loaded at boot (modules-load.d)"),
     QT_TRANSLATE_NOOP("ConfigPage", "One module per line, loaded at every boot (OpenRC: the modules-load service; systemd: systemd-modules-load)."), Kind::ModulesLoad},
    {"udev", "/etc/udev/rules.d/99-centurion-user.rules", QT_TRANSLATE_NOOP("ConfigPage", "udev rules (Centurion's file)"),
     QT_TRANSLATE_NOOP("ConfigPage", "Rules are reloaded on save and apply to devices added from then on (or after a reboot)."), Kind::Udev},
    {"sysctl_conf", "/etc/sysctl.conf", QT_TRANSLATE_NOOP("ConfigPage", "System sysctl.conf"),
     QT_TRANSLATE_NOOP("ConfigPage", "The distribution's own file: applied at once (sysctl -p) and at every boot."), Kind::Sysctl},
    {"fstab", "/etc/fstab", QT_TRANSLATE_NOOP("ConfigPage", "Filesystems mounted at boot (fstab)"),
     QT_TRANSLATE_NOOP("ConfigPage", "Checked with findmnt --verify before it is saved; nothing is mounted. A wrong entry can still stop the machine "
                                     "from booting normally: keep a live USB at hand when changing the root or boot lines."), Kind::Fstab},
    {"openrc_modules", "/etc/conf.d/modules", QT_TRANSLATE_NOOP("ConfigPage", "Modules loaded at boot (OpenRC, /etc/conf.d/modules)"),
     QT_TRANSLATE_NOOP("ConfigPage", "Gentoo / OpenRC: modules=\"…\" lists the modules the modules service loads at boot, module_<name>_args=\"…\" "
                                     "their options. OpenRC reads the file as a shell script, so only plain NAME=\"value\" lines are accepted."), Kind::ShellVars},
};

// Same rule as sysadmin::modprobe_d_name_ok: a plain *.conf name, not Centurion's own drop-in.
bool modprobeDName(const QString &n) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}\\.conf$"));
    return re.match(n).hasMatch() && !n.contains(QLatin1String("..")) && n != QLatin1String("centurion-user.conf");
}

// The fixed files, minus /etc/conf.d/modules where OpenRC is not used, plus every other existing *.conf in
// /etc/modprobe.d (id "modprobe_d.<name>"; the helper re-checks the name and that the file exists).
const std::vector<ConfigPage::File> &list() {
    static std::vector<ConfigPage::File> all;
    static QList<QByteArray> strings;  // keeps the dynamic ids, paths and labels alive
    if (!all.empty()) return all;
    for (const ConfigPage::File &f : FILES)
        if (f.kind != Kind::ShellVars || QFileInfo(QLatin1String(f.path)).isFile()) all.push_back(f);
    const QFileInfoList entries = QDir(QStringLiteral("/etc/modprobe.d")).entryInfoList({QStringLiteral("*.conf")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (!modprobeDName(fi.fileName()) || all.size() >= 200) continue;
        strings << ("modprobe_d." + fi.fileName().toUtf8()) << fi.absoluteFilePath().toUtf8() << ("modprobe.d: " + fi.fileName().toUtf8());
        all.push_back({strings.at(strings.size() - 3).constData(), strings.at(strings.size() - 2).constData(), strings.last().constData(),
                       QT_TRANSLATE_NOOP("ConfigPage", "A file in /etc/modprobe.d. Takes effect the next time the module loads; for drivers in the initramfs, regenerate it (dracut) and reboot."),
                       Kind::Modprobe});
    }
    return all;
}
const ConfigPage::File *files() { return list().data(); }
int fileCount() { return int(list().size()); }

}  // namespace

QString ConfigPage::diff(const QString &before, const QString &after) {
    const QStringList a = before.split(QLatin1Char('\n')), b = after.split(QLatin1Char('\n'));
    // Common head and tail first; the LCS runs on the changed middle only.
    int head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    int tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    const int n = int(a.size()) - head - tail, m = int(b.size()) - head - tail;
    QStringList lines;  // "  x", "- x", "+ x"
    for (int i = 0; i < head; ++i) lines << QStringLiteral("  ") + a[i];
    if (qint64(n) * m > 4'000'000) {
        for (int i = 0; i < n; ++i) lines << QStringLiteral("- ") + a[head + i];
        for (int j = 0; j < m; ++j) lines << QStringLiteral("+ ") + b[head + j];
    } else {
        std::vector<int> L(size_t(n + 1) * size_t(m + 1), 0);
        auto at = [&](int i, int j) -> int & { return L[size_t(i) * size_t(m + 1) + size_t(j)]; };
        for (int i = n - 1; i >= 0; --i)
            for (int j = m - 1; j >= 0; --j)
                at(i, j) = a[head + i] == b[head + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        int i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a[head + i] == b[head + j]) { lines << QStringLiteral("  ") + a[head + i]; ++i; ++j; }
            else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) lines << QStringLiteral("- ") + a[head + i++];  // old line first
            else lines << QStringLiteral("+ ") + b[head + j++];
        }
    }
    for (int i = int(a.size()) - tail; i < a.size(); ++i) lines << QStringLiteral("  ") + a[i];
    // Long unchanged runs shortened to 2 lines of context on each side.
    QStringList out;
    for (int i = 0; i < lines.size();) {
        int k = i;
        while (k < lines.size() && lines[k].startsWith(QLatin1String("  "))) ++k;
        const int run = k - i;
        if (run > 6) {
            const bool first = i == 0, last = k == lines.size();
            for (int x = 0; x < (first ? 0 : 2); ++x) out << lines[i + x];
            out << tr("  … %n unchanged line(s)", nullptr, run - (first ? 0 : 2) - (last ? 0 : 2));
            for (int x = last ? run : run - 2; x < run; ++x) out << lines[i + x];
        } else {
            for (int x = i; x < k; ++x) out << lines[x];
        }
        if (k < lines.size()) out << lines[k++];
        i = k;
    }
    return out.join(QLatin1Char('\n'));
}

ConfigPage::ConfigPage(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 6);
    auto *about = new QLabel(tr("System configuration files, edited here and saved with the administrator password. Every save is "
                                "checked first, shows what changes, and keeps the previous version (the last 10) for Restore."));
    about->setWordWrap(true);
    about->setProperty("role", "muted");
    v->addWidget(about);
    auto *top = new QHBoxLayout;
    files_ = new QComboBox;
    for (int i = 0; i < fileCount(); ++i) files_->addItem(tr(files()[i].label));
    top->addWidget(files_, 1);
    auto *reload = new QPushButton(tr("Reload"));
    reload->setToolTip(tr("Read the file from disk again (drops unsaved changes)."));
    restore_ = new QPushButton(tr("Restore backup…"));
    save_ = new QPushButton(tr("Save…"));
    top->addWidget(reload);
    top->addWidget(restore_);
    top->addWidget(save_);
    v->addLayout(top);
    path_ = new QLabel;
    path_->setWordWrap(true);
    path_->setProperty("role", "muted");
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(path_);

    tabs_ = new QTabWidget;
    tableBox_ = new QWidget;
    auto *tv = new QVBoxLayout(tableBox_);
    tv->setContentsMargins(0, 4, 0, 0);
    table_ = new QTableWidget;
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    tv->addWidget(table_, 1);
    auto *rowBar = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add…"));
    auto *remove = new QPushButton(tr("Remove selected"));
    rowBar->addWidget(add);
    rowBar->addWidget(remove);
    rowBar->addStretch(1);
    tv->addLayout(rowBar);
    tabs_->addTab(tableBox_, tr("Settings"));
    text_ = new QPlainTextEdit;
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs_->addTab(text_, tr("Text"));
    v->addWidget(tabs_, 1);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(status_);

    int shown = 0;
    connect(files_, &QComboBox::currentIndexChanged, this, [this, shown](int i) mutable {
        if (i == shown) return;
        if (!confirmLeave()) { QSignalBlocker b(files_); files_->setCurrentIndex(shown); return; }
        shown = i;
        load();
    });
    connect(reload, &QPushButton::clicked, this, [this] { if (confirmLeave()) load(); });
    connect(save_, &QPushButton::clicked, this, &ConfigPage::save);
    connect(restore_, &QPushButton::clicked, this, &ConfigPage::restore);
    connect(add, &QPushButton::clicked, this, &ConfigPage::addRow);
    connect(remove, &QPushButton::clicked, this, [this] {
        QList<int> rows;
        for (const QModelIndex &i : table_->selectionModel()->selectedRows()) rows << i.row();
        std::sort(rows.begin(), rows.end(), std::greater<>());
        for (int r : rows) table_->removeRow(r);
        tableToText();
    });
    connect(table_, &QTableWidget::itemChanged, this, [this] { if (!syncing_) { tableToText(); fillLive(); } });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (tabs_->widget(i) == tableBox_) textToTable();
    });
    load();
}

const ConfigPage::File &ConfigPage::file() const { return files()[std::clamp(files_->currentIndex(), 0, fileCount() - 1)]; }

bool ConfigPage::confirmLeave() {
    if (text_->toPlainText() == disk_) return true;
    return QMessageBox::question(this, tr("Unsaved changes"), tr("Drop the unsaved changes to %1?").arg(QLatin1String(file().path)),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard;
}

void ConfigPage::load() {
    const File &f = file();
    bool ok = true;
    disk_ = readText(QLatin1String(f.path), &ok);
    const bool exists = QFile::exists(QLatin1String(f.path));
    path_->setText(QStringLiteral("%1 — %2%3").arg(QLatin1String(f.path), tr(f.note),
                                                   exists ? QString() : QStringLiteral(" ") + tr("(does not exist yet: saving creates it)")));
    status_->clear();
    if (!ok) {
        status_->setText(tr("%1 cannot be read by this user.").arg(QLatin1String(f.path)));
        status_->setStyleSheet(QStringLiteral("color:%1").arg(theme::DANGER));
    }
    {
        QSignalBlocker b(text_);
        text_->setPlainText(disk_);
    }
    const bool table = structured(f.kind);
    tabs_->setTabEnabled(tabs_->indexOf(tableBox_), table);
    tabs_->setCurrentWidget(table ? static_cast<QWidget *>(tableBox_) : text_);
    textToTable();
    QDir d(backupDir(f.id));
    restore_->setEnabled(!d.entryList({QStringLiteral("*.bak")}, QDir::Files).isEmpty());
    save_->setEnabled(ok);
}

void ConfigPage::textToTable() {
    const File &f = file();
    if (!structured(f.kind)) return;
    syncing_ = true;
    base_ = text_->toPlainText().split(QLatin1Char('\n'));
    const QStringList hdr = headers(f.kind);
    table_->clear();
    table_->setColumnCount(int(hdr.size()));
    table_->setHorizontalHeaderLabels(hdr);
    table_->setRowCount(0);
    for (int i = 0; i < base_.size(); ++i) {
        const QStringList cells = parse(f.kind, base_[i]);
        if (cells.isEmpty()) continue;
        const int r = table_->rowCount();
        table_->insertRow(r);
        for (int c = 0; c < cells.size(); ++c) table_->setItem(r, c, new QTableWidgetItem(cells[c]));
        table_->item(r, 0)->setData(Qt::UserRole, i);
    }
    syncing_ = false;
    fillLive();
    table_->resizeColumnsToContents();
}

// The live column (sysctl: the kernel's value now; modules-load: loaded or not). Read only.
void ConfigPage::fillLive() {
    const Kind k = file().kind;
    if (k != Kind::Sysctl && k != Kind::ModulesLoad) return;
    const bool was = syncing_;
    syncing_ = true;
    const int col = k == Kind::Sysctl ? 2 : 1;
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QString key = table_->item(r, 0) ? table_->item(r, 0)->text().trimmed() : QString();
        QString now;
        if (k == Kind::Sysctl) now = sysctlNow(key);
        else now = QDir(QStringLiteral("/sys/module/") + QString(key).replace(QLatin1Char('-'), QLatin1Char('_'))).exists() ? tr("yes") : tr("no");
        auto *it = new QTableWidgetItem(now);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        if (k == Kind::Sysctl && table_->item(r, 1) && now != QLatin1String("—") && now != table_->item(r, 1)->text().simplified())
            it->setForeground(QColor(theme::WARN));
        table_->setItem(r, col, it);
    }
    syncing_ = was;
}

void ConfigPage::tableToText() {
    const File &f = file();
    if (!structured(f.kind)) return;
    const int cells = f.kind == Kind::Sysctl ? 2 : f.kind == Kind::Modprobe ? 3 : 1;
    QHash<int, QString> byLine;
    QStringList added;
    for (int r = 0; r < table_->rowCount(); ++r) {
        QStringList c;
        for (int i = 0; i < cells; ++i) c << (table_->item(r, i) ? table_->item(r, i)->text() : QString());
        if (c.value(0).trimmed().isEmpty()) continue;
        const int line = table_->item(r, 0) ? table_->item(r, 0)->data(Qt::UserRole).toInt() : -1;
        const QString text = format(f.kind, c);
        if (line >= 0 && table_->item(r, 0)->data(Qt::UserRole).isValid()) byLine.insert(line, text); else added << text;
    }
    // Comments and lines the table does not show stay where they were; removed rows drop their line.
    QStringList out;
    for (int i = 0; i < base_.size(); ++i) {
        if (parse(f.kind, base_[i]).isEmpty()) out << base_[i];
        else if (byLine.contains(i)) out << byLine.value(i);
    }
    while (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
    out << added;
    QString t = out.join(QLatin1Char('\n'));
    if (!t.isEmpty()) t += QLatin1Char('\n');
    QSignalBlocker b(text_);
    text_->setPlainText(t);
}

void ConfigPage::addRow() {
    const File &f = file();
    QDialog d(this);
    d.setWindowTitle(tr("Add to %1").arg(QLatin1String(f.path)));
    auto *v = new QVBoxLayout(&d);
    auto *row = new QHBoxLayout;
    QComboBox *cmd = nullptr;
    if (f.kind == Kind::Modprobe) {
        cmd = new QComboBox;
        cmd->addItems({QStringLiteral("options"), QStringLiteral("blacklist")});
        row->addWidget(cmd);
    }
    auto *first = new QLineEdit;
    first->setPlaceholderText(f.kind == Kind::Sysctl ? tr("key, e.g. vm.swappiness") : tr("module"));
    auto *comp = new QCompleter(f.kind == Kind::Sysctl ? sysctlKeys() : moduleNames(), first);
    comp->setCaseSensitivity(Qt::CaseInsensitive);
    comp->setFilterMode(Qt::MatchContains);
    first->setCompleter(comp);
    first->setMinimumWidth(260);
    row->addWidget(first, 1);
    QLineEdit *second = nullptr;
    if (f.kind != Kind::ModulesLoad) {
        second = new QLineEdit;
        second->setPlaceholderText(f.kind == Kind::Sysctl ? tr("value") : tr("parameters, e.g. modeset=1"));
        row->addWidget(second, 1);
        if (f.kind == Kind::Sysctl)
            connect(first, &QLineEdit::textChanged, second, [second](const QString &k) { second->setPlaceholderText(tr("value (now: %1)").arg(sysctlNow(k.trimmed()))); });
        if (cmd) connect(cmd, &QComboBox::currentTextChanged, second, [second](const QString &c) { second->setEnabled(c == QLatin1String("options")); });
    }
    v->addLayout(row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted || first->text().trimmed().isEmpty()) return;
    QStringList cells;
    if (cmd) cells << cmd->currentText();
    cells << first->text().trimmed();
    if (second && second->isEnabled()) cells << second->text().trimmed();
    syncing_ = true;
    int r = -1;
    if (f.kind == Kind::Sysctl)  // one row per key: an existing key gets the new value
        for (int i = 0; i < table_->rowCount(); ++i)
            if (table_->item(i, 0) && table_->item(i, 0)->text().trimmed() == cells[0]) r = i;
    if (r < 0) {
        r = table_->rowCount();
        table_->insertRow(r);
        for (int c = 0; c < cells.size(); ++c) table_->setItem(r, c, new QTableWidgetItem(cells[c]));
    } else {
        table_->setItem(r, 1, new QTableWidgetItem(cells.value(1)));
    }
    syncing_ = false;
    tableToText();
    fillLive();
    table_->resizeColumnsToContents();
}

namespace {
/// The difference in a dialog; true when the user confirms.
bool confirmDiff(QWidget *parent, const QString &title, const QString &intro, const QString &diffText, const QString &button) {
    QDialog d(parent);
    d.setWindowTitle(title);
    d.resize(720, 480);
    auto *v = new QVBoxLayout(&d);
    auto *l = new QLabel(intro);
    l->setWordWrap(true);
    v->addWidget(l);
    auto *t = new QPlainTextEdit(diffText);
    t->setReadOnly(true);
    t->setLineWrapMode(QPlainTextEdit::NoWrap);
    t->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // Removed lines red, added lines green.
    QList<QTextEdit::ExtraSelection> marks;
    for (QTextBlock b = t->document()->begin(); b.isValid(); b = b.next()) {
        const QString s = b.text();
        if (!s.startsWith(QLatin1String("- ")) && !s.startsWith(QLatin1String("+ "))) continue;
        QTextEdit::ExtraSelection m;
        m.cursor = QTextCursor(b);
        m.format.setProperty(QTextFormat::FullWidthSelection, true);
        m.format.setForeground(QColor(s.startsWith(QLatin1Char('-')) ? theme::DANGER : theme::OK));
        marks << m;
    }
    t->setExtraSelections(marks);
    v->addWidget(t, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Cancel);
    bb->addButton(button, QDialogButtonBox::AcceptRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    return d.exec() == QDialog::Accepted;
}
}  // namespace

void ConfigPage::save() {
    const File &f = file();
    if (tabs_->currentWidget() == tableBox_) tableToText();
    QString text = text_->toPlainText();
    if (text == disk_) { status_->setStyleSheet(QString()); status_->setText(tr("No changes to save.")); return; }
    const QString intro = tr("%1 will be changed as below. The current version is kept as a backup.").arg(QLatin1String(f.path)) +
                          (f.kind == Kind::Fstab ? QStringLiteral("\n\n") + tr(f.note) : QString());
    if (!confirmDiff(this, tr("Save %1").arg(QLatin1String(f.path)), intro, diff(disk_, text), tr("Save")))
        return;
    save_->setEnabled(false);
    status_->setStyleSheet(QString());
    status_->setText(tr("Saving…"));
    const QString id = QLatin1String(f.id);
    writeConfig(id, text, false);
}

void ConfigPage::writeConfig(const QString &id, const QString &text, bool confirmFstab) {
    QJsonObject req{{QStringLiteral("op"), QStringLiteral("config_write")}, {QStringLiteral("id"), id}, {QStringLiteral("content"), text}};
    if (confirmFstab) req.insert(QStringLiteral("confirm_fstab"), true);
    sysadmin::runJson(req, this, [this, id, text](bool ok, const QString &msg, const QJsonObject &reply) {
        save_->setEnabled(true);
        if (QLatin1String(file().id) != id) return;  // another file was opened meanwhile
        // fstab: the helper compared the changed lines with the disks present now (fstabcheck.rs).
        if (!ok && reply.value(QStringLiteral("needs_confirm")).toBool()) {
            QStringList issues;
            for (const QJsonValue &v : reply.value(QStringLiteral("fstab_issues")).toArray()) issues << QStringLiteral("• ") + v.toString();
            const auto answer = QMessageBox::warning(this, tr("Check /etc/fstab"),
                tr("These changed lines do not match the disks on this machine. A wrong entry can stop the next boot "
                   "at an emergency shell.\n\n%1\n\nSave anyway?").arg(issues.join(QLatin1Char('\n'))),
                QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer == QMessageBox::Save) {
                save_->setEnabled(false);
                status_->setStyleSheet(QString());
                status_->setText(tr("Saving…"));
                writeConfig(id, text, true);
                return;
            }
            status_->setStyleSheet(QStringLiteral("color:%1").arg(theme::DANGER));
            status_->setText(tr("Not saved: the fstab check found problems."));
            return;
        }
        status_->setStyleSheet(QStringLiteral("color:%1").arg(ok ? theme::OK : theme::DANGER));
        if (ok) { load(); status_->setStyleSheet(QStringLiteral("color:%1").arg(theme::OK)); }
        status_->setText((ok ? tr("Saved. ") : tr("Not saved: ")) + msg);
    });
}

void ConfigPage::restore() {
    const File &f = file();
    if (!confirmLeave()) return;
    QStringList names = QDir(backupDir(f.id)).entryList({QStringLiteral("*.bak")}, QDir::Files, QDir::Name | QDir::Reversed);
    if (names.isEmpty()) { QMessageBox::information(this, tr("Restore backup"), tr("No backup of %1 yet.").arg(QLatin1String(f.path))); return; }
    QStringList labels;
    for (const QString &n : names)
        labels << QDateTime::fromMSecsSinceEpoch(n.chopped(4).toLongLong()).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("Restore backup"), tr("Version of %1 saved before the change at:").arg(QLatin1String(f.path)),
                                               labels, 0, false, &ok);
    if (!ok) return;
    const QString name = names.value(labels.indexOf(pick));
    const QString old = readText(backupDir(f.id) + QLatin1Char('/') + name);
    if (!confirmDiff(this, tr("Restore %1").arg(QLatin1String(f.path)), tr("%1 goes back to the version from %2. The current version is kept as a backup too.")
                         .arg(QLatin1String(f.path), pick), diff(disk_, old), tr("Restore")))
        return;
    const QString id = QLatin1String(f.id);
    sysadmin::run(QJsonObject{{QStringLiteral("op"), QStringLiteral("config_restore")}, {QStringLiteral("id"), id}, {QStringLiteral("backup"), name}}, this,
                  [this, id](bool ok, const QString &msg) {
        if (QLatin1String(file().id) != id) return;
        if (ok) load();
        status_->setStyleSheet(QStringLiteral("color:%1").arg(ok ? theme::OK : theme::DANGER));
        status_->setText((ok ? QString() : tr("Not restored: ")) + msg);
    });
}
