#include "diagreportview.h"

#include "sysadmin.h"
#include "theme.h"
#include "toolcatalog.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QComboBox>
#include <QCheckBox>
#include <QSplitter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSaveFile>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

using diag::Level;

namespace {

QSettings guiSettings() {
    return QSettings(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/gui.ini"), QSettings::IniFormat);
}

const char *color(Level l) {
    switch (l) {
    case Level::Attention: return theme::DANGER;
    case Level::Review: return theme::WARN;
    case Level::Ok: return theme::OK;
    case Level::Unknown: return theme::MUTED;
    case Level::Info: break;
    }
    return theme::FG_DIM;
}

QLabel *text(const QString &s, const char *tone = nullptr) {
    auto *l = new QLabel(s);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (tone) l->setProperty("tone", tone);
    return l;
}

QWidget *findingCard(const diag::Finding &f, DiagReportView *view) {
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("diagCard"));
    card->setStyleSheet(QStringLiteral("QFrame#diagCard{background:%1;border:1px solid %2;border-left:3px solid %3;border-radius:8px}"
                                       "QFrame#diagCard QLabel{background:transparent;border:none}")
                            .arg(theme::BG2, theme::BORDER, color(f.level)));
    card->setProperty("diagLevel", int(f.level));
    card->setProperty("diagSearch", f.title + QLatin1Char('\n') + f.detail + QLatin1Char('\n') + f.action + QLatin1Char('\n') + f.fixLabel);
    auto *col = new QVBoxLayout(card);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(4);
    auto *head = new QHBoxLayout;
    auto *title = text(f.title);
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    head->addWidget(title, 1);
    auto *badge = new QLabel(diag::levelName(f.level));
    badge->setStyleSheet(QStringLiteral("color:%1;font-weight:600").arg(color(f.level)));
    head->addWidget(badge, 0, Qt::AlignTop);
    col->addLayout(head);
    if (!f.detail.isEmpty()) col->addWidget(text(f.detail, "dim"));
    if (!f.action.isEmpty()) col->addWidget(text(QStringLiteral("→ ") + f.action));
    // Technical terms in the finding: named on one quiet line, explained on hover.
    const QString terms = toolcatalog::explainTerms(f.title + QLatin1Char(' ') + f.detail + QLatin1Char(' ') + f.action);
    if (!terms.isEmpty()) {
        QStringList names;
        for (const QString &line : terms.split(QLatin1Char('\n'))) names << line.section(QStringLiteral(" — "), 0, 0);
        auto *glossary = text(QCoreApplication::translate("diag", "ⓘ Terms: %1").arg(names.join(QStringLiteral(" · "))), "muted");
        glossary->setToolTip(terms);
        col->addWidget(glossary);
    }
    if (!f.fix.isEmpty()) {
        auto *fix = new QPushButton(f.fixLabel);
        fix->setToolTip(QCoreApplication::translate("diag", "Makes this change now (asks for the administrator password)."));
        QObject::connect(fix, &QPushButton::clicked, view, [view, f] {
            sysadmin::act(view, f.fixLabel, QCoreApplication::translate("diag", "%1\n\n%2?").arg(f.title, f.fixLabel), f.fix,
                          [view](bool ok) { if (ok) Q_EMIT view->fixApplied(); });
        });
        col->addWidget(fix, 0, Qt::AlignLeft);
    }
    return card;
}

QWidget *tableWidget(const diag::Table &t) {
    auto *box = new QWidget;
    auto *col = new QVBoxLayout(box);
    col->setContentsMargins(0, 4, 0, 0);
    col->setSpacing(3);
    auto *title = text(t.title);
    title->setWordWrap(false);  // a wrapping label in a scroll area reserves height it does not use
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    col->addWidget(title);
    if (t.rows.isEmpty()) {
        col->addWidget(text(QCoreApplication::translate("diag", "(nothing to list)"), "muted"));
    } else {
        auto *tree = new QTreeWidget;
        int columns = qMax(1, int(t.headers.size()));
        for (const QStringList &row : t.rows) columns = qMax(columns, int(row.size()));
        tree->setColumnCount(columns);
        tree->setHeaderLabels(t.headers);
        tree->setHeaderHidden(t.headers.isEmpty());
        tree->setRootIsDecorated(false);
        tree->setAlternatingRowColors(true);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree->setUniformRowHeights(true);
        tree->setWordWrap(false);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setSortingEnabled(false);
        for (int i = 0; i < t.rows.size(); ++i) {
            auto *it = new QTreeWidgetItem(t.rows[i]);
            for (int c = 0; c < t.rows[i].size(); ++c) it->setToolTip(c, t.rows[i][c]);
            const Level l = i < t.levels.size() ? t.levels[i] : Level::Info;
            if (l == Level::Attention || l == Level::Review || l == Level::Ok) it->setForeground(0, QColor(color(l)));
            tree->addTopLevelItem(it);
        }
        for (int c = 0; c < tree->columnCount(); ++c) {
            tree->resizeColumnToContents(c);
            tree->setColumnWidth(c, qMin(tree->columnWidth(c), tree->fontMetrics().horizontalAdvance(QString(80, QLatin1Char('M')))));
        }
        tree->header()->setStretchLastSection(true);
        // Preserve diagnostic order until a user explicitly requests sorting.
        tree->header()->setSectionsClickable(true);
        QObject::connect(tree->header(), &QHeaderView::sectionClicked, tree, [tree](int column) {
            if (!tree->isSortingEnabled()) {
                tree->setSortingEnabled(true);
                tree->sortByColumn(column, Qt::AscendingOrder);
            }
        });
        // Sized to its rows (up to 14) so the page scrolls as one, not a stack of small scroll boxes.
        const int rowH = tree->sizeHintForRow(0) > 0 ? tree->sizeHintForRow(0) : tree->fontMetrics().height() + 6;
        const int shown = qMin(int(t.rows.size()), 14);
        // Room for a horizontal scroll bar: wide tables (many columns, long values) get one, and it must not cover the last row.
        tree->setFixedHeight(rowH * shown + (t.headers.isEmpty() ? 0 : tree->header()->sizeHint().height()) + 2 * tree->frameWidth() + 4 +
                             tree->horizontalScrollBar()->sizeHint().height());
        col->addWidget(tree);
    }
    if (!t.note.isEmpty()) col->addWidget(text(t.note, "muted"));
    return box;
}

}  // namespace

DiagReportView::DiagReportView(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto *top = new QHBoxLayout;
    verdict_ = new QLabel;
    verdict_->setStyleSheet(QStringLiteral("font-weight:700"));
    headline_ = new QLabel;
    headline_->setWordWrap(true);
    headline_->setTextFormat(Qt::PlainText);
    top->addWidget(verdict_);
    top->addWidget(headline_, 1);
    copy_ = new QPushButton(tr("Copy report"));
    save_ = new QPushButton(tr("Save report…"));
    copy_->setEnabled(false);
    save_->setEnabled(false);
    v->addLayout(top);
    auto *actions = new QHBoxLayout;
    snapshot_ = text(QString(), "muted");
    actions->addWidget(snapshot_, 1);
    preview_ = new QPushButton(tr("Export preview…"));
    actions->addWidget(preview_);
    pin_ = new QPushButton(tr("Pin Snapshot")); compare_ = new QPushButton(tr("Compare Snapshot…"));
    pin_->setEnabled(false); compare_->setEnabled(false);
    actions->addWidget(pin_); actions->addWidget(compare_);
    connect(pin_, &QPushButton::clicked, this, [this] {
        if (!hasReport_) return;
        baselineCode_ = report_.code; baselineText_ = report_.toText(); baselineTime_ = snapshot_->text();
        compare_->setEnabled(true);
    });
    connect(compare_, &QPushButton::clicked, this, [this] {
        if (!hasReport_ || baselineCode_ != report_.code || baselineText_.isEmpty()) return;
        QDialog dlg(this); dlg.setWindowTitle(tr("Diagnostic Snapshot Comparison")); dlg.resize(1000, 650);
        auto *layout = new QVBoxLayout(&dlg);
        const auto current = report_.toText();
        layout->addWidget(text(current == baselineText_ ? tr("Reports are textually identical.") : tr("Reports differ. Differences are not proof of a hardware change.")));
        auto *split = new QSplitter(Qt::Horizontal);
        for (const auto &entry : {qMakePair(baselineTime_,baselineText_),qMakePair(snapshot_->text(),current)}) {
            auto *panel = new QWidget; auto *v = new QVBoxLayout(panel); v->addWidget(text(entry.first));
            auto *raw = new QPlainTextEdit(entry.second); raw->setReadOnly(true); raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); v->addWidget(raw); split->addWidget(panel);
        }
        layout->addWidget(split,1); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject); layout->addWidget(buttons); dlg.exec();
    });
    actions->addWidget(copy_);
    actions->addWidget(save_);
    v->addLayout(actions);
    connect(preview_, &QPushButton::clicked, this, &DiagReportView::previewExport);
    about_ = text(QString(), "muted");
    v->addWidget(about_);
    auto *filters = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search findings and readings…"));
    search_->setClearButtonEnabled(true);
    search_->setToolTip(tr("Filters findings and table rows only. The verdict, limits and full export are unchanged."));
    filters->addWidget(search_, 1);
    severity_ = new QComboBox;
    severity_->addItem(tr("All finding levels"), -1);
    for (Level l : {Level::Attention, Level::Review, Level::Unknown, Level::Info, Level::Ok})
        severity_->addItem(diag::levelName(l), int(l));
    filters->addWidget(severity_);
    sections_ = new QComboBox;
    sections_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sections_->setMinimumContentsLength(12);
    filters->addWidget(sections_, 1);
    v->addLayout(filters);
    filterInfo_ = text(QString(), "muted");
    v->addWidget(filterInfo_);
    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    body_ = new QWidget;
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(0, 2, 6, 6);
    bodyLayout_->setSpacing(8);
    scroll_->setWidget(body_);
    v->addWidget(scroll_, 1);
    connect(search_, &QLineEdit::textChanged, this, &DiagReportView::filterReport);
    connect(severity_, &QComboBox::currentIndexChanged, this, &DiagReportView::filterReport);
    connect(sections_, &QComboBox::activated, this, [this](int i) {
        if (i > 0 && i <= sectionWidgets_.size()) {
            QWidget *target = sectionWidgets_.at(i - 1);
            if (findingWidgets_.contains(target) && target->isHidden()) {
                for (QWidget *w : findingWidgets_) if (!w->isHidden()) { target = w; break; }
            }
            if (!target->isHidden()) scroll_->ensureWidgetVisible(target, 0, 8);
        }
    });

    connect(copy_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(report_.toText()); });
    connect(save_, &QPushButton::clicked, this, [this] {
        const QString name = QStringLiteral("centurion-%1-%2.txt")
                                 .arg(report_.code.toLower(), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
        const QByteArray data = report_.toText().toUtf8();
        const QString path = QFileDialog::getSaveFileName(this, tr("Save report"), QDir::homePath() + QLatin1Char('/') + name,
                                                          tr("Text files (*.txt)"));
        if (path.isEmpty()) return;
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
            headline_->setText(tr("Could not save %1: %2").arg(path, f.errorString()));
    });
    setMessage(QString());
}

void DiagReportView::clearBody() {
    findingWidgets_.clear();
    sectionWidgets_.clear();
    tables_.clear();
    sections_->clear();
    while (QLayoutItem *item = bodyLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void DiagReportView::setMessage(const QString &s) {
    hasReport_ = false;
    report_ = {};
    clearBody();
    search_->clear();
    severity_->setCurrentIndex(0);
    for (QWidget *w : {static_cast<QWidget *>(search_), static_cast<QWidget *>(severity_), static_cast<QWidget *>(sections_)}) w->setEnabled(false);
    snapshot_->clear();
    filterInfo_->clear();
    preview_->setEnabled(false); pin_->setEnabled(false); compare_->setEnabled(false);
    verdict_->clear();
    verdict_->setVisible(false);
    headline_->setText(s);
    about_->setVisible(false);
    copy_->setEnabled(false);
    save_->setEnabled(false);
    bodyLayout_->addStretch();
}

void DiagReportView::setReport(const diag::Report &r) {
    // Preserve position and filters only for a replacement of the same visible report.
    const bool same = hasReport_ && report_.code == r.code;
    const int oldScroll = same ? scroll_->verticalScrollBar()->value() : 0;
    const diag::Report replacement = r;  // caller may pass report() itself
    clearBody();
    report_ = replacement;
    const diag::Report &current = report_;
    hasReport_ = true;
    if (!same) {
        const QSignalBlocker b1(search_), b2(severity_);
        search_->clear();
        severity_->setCurrentIndex(0);
    }
    search_->setEnabled(true);
    severity_->setEnabled(true);
    sections_->setEnabled(true);
    preview_->setEnabled(true); pin_->setEnabled(true); compare_->setEnabled(baselineCode_ == report_.code && !baselineText_.isEmpty());
    snapshot_->setText(tr("Report displayed: %1 · snapshot, not live data").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    sections_->addItem(tr("Jump to section…"));
    auto addSection = [this](const QString &name, QWidget *w) {
        sections_->addItem(name);
        sectionWidgets_ << w;
    };
    const Level v = current.verdict();
    verdict_->setText(diag::levelName(v));
    verdict_->setStyleSheet(QStringLiteral("font-weight:700;color:%1").arg(color(v)));
    verdict_->setVisible(true);
    headline_->setText(QStringLiteral("· ") + current.headline());
    about_->setText(current.about);
    about_->setVisible(!current.about.isEmpty());
    copy_->setEnabled(true);
    save_->setEnabled(true);

    // Most serious first, the order within a level as the diagnostic wrote it.
    QList<diag::Finding> findings = current.findings;
    std::stable_sort(findings.begin(), findings.end(), [](const diag::Finding &a, const diag::Finding &b) { return a.level > b.level; });
    for (const diag::Finding &f : findings) {
        auto *card = findingCard(f, this);
        if (findingWidgets_.isEmpty()) addSection(tr("Findings"), card);
        findingWidgets_ << card;
        bodyLayout_->addWidget(card);
    }
    for (const diag::Table &t : current.tables) {
        auto *w = tableWidget(t);
        if (auto *tree = w->findChild<QTreeWidget *>()) tables_ << tree;
        addSection(t.title, w);
        bodyLayout_->addWidget(w);
    }
    if (!current.limits.isEmpty()) {
        auto *lim = text(tr("Limits of this check:") + QStringLiteral("\n• ") + current.limits.join(QStringLiteral("\n• ")), "muted");
        addSection(tr("Limits of this check:"), lim);
        bodyLayout_->addWidget(lim);
    }
    if (!current.raw.trimmed().isEmpty()) {
        auto *toggle = new QPushButton(tr("Show technical details"));
        toggle->setCheckable(true);
        auto *raw = new QPlainTextEdit(current.raw.trimmed());
        raw->setReadOnly(true);
        raw->setLineWrapMode(QPlainTextEdit::NoWrap);
        raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        raw->setMinimumHeight(220);
        raw->setVisible(false);
        connect(toggle, &QPushButton::toggled, this, [toggle, raw](bool on) {
            raw->setVisible(on);
            toggle->setText(on ? tr("Hide technical details") : tr("Show technical details"));
        });
        // The choice is remembered for every report: who reads the raw output once usually wants it again.
        connect(toggle, &QPushButton::clicked, this, [](bool on) { guiSettings().setValue(QStringLiteral("diag/technical"), on); });
        toggle->setChecked(guiSettings().value(QStringLiteral("diag/technical"), false).toBool());
        addSection(tr("Technical details"), toggle);
        bodyLayout_->addWidget(toggle, 0, Qt::AlignLeft);
        bodyLayout_->addWidget(raw);
    }
    bodyLayout_->addStretch();
    filterReport();
    bodyLayout_->activate();
    scroll_->verticalScrollBar()->setValue(oldScroll);
}

void DiagReportView::filterReport() {
    if (!hasReport_) return;
    const QString needle = search_->text().trimmed();
    const int level = severity_->currentData().toInt();
    int visibleFindings = 0, visibleRows = 0, allRows = 0;
    for (QWidget *w : findingWidgets_) {
        const bool match = (level < 0 || w->property("diagLevel").toInt() == level)
            && w->property("diagSearch").toString().contains(needle, Qt::CaseInsensitive);
        w->setVisible(match);
        visibleFindings += match;
    }
    // Keep tables, notes and limits available even when all findings are filtered out.
    for (QTreeWidget *tree : tables_) {
        int visible = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *item = tree->topLevelItem(i);
            bool match = needle.isEmpty();
            for (int c = 0; c < tree->columnCount() && !match; ++c)
                match = item->text(c).contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            visibleRows += match;
            visible += match;
            ++allRows;
        }
        const int rowH = tree->fontMetrics().height() + 8;
        tree->setFixedHeight(rowH * qBound(1, visible, 14) + (tree->isHeaderHidden() ? 0 : tree->header()->sizeHint().height())
                             + 2 * tree->frameWidth() + 4 + tree->horizontalScrollBar()->sizeHint().height());
    }
    filterInfo_->setText(tr("Showing %1/%2 findings and %3/%4 table rows. Export includes the full report.")
        .arg(visibleFindings).arg(findingWidgets_.size()).arg(visibleRows).arg(allRows));
}

void DiagReportView::previewExport() {
    if (!hasReport_) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Export preview"));
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(text(tr("Reports may contain private data. Automatic redaction is best effort only. Review and edit the preview before sharing.")));
    const diag::Report captured = report_;
    const QString fullReport = report_.toText();
    auto *raw = new QPlainTextEdit(fullReport);
    auto *selection = new QTreeWidget; selection->setHeaderHidden(true);
    selection->setMaximumHeight(fontMetrics().height() * 9);
    auto add = [](QTreeWidgetItem *parent, const QString &label) {
        auto *item = new QTreeWidgetItem(parent, {label});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked); return item;
    };
    auto *findings = new QTreeWidgetItem(selection, {tr("Findings")});
    findings->setFlags(findings->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
    for (const auto &f : captured.findings) add(findings, f.title);
    auto *tables = new QTreeWidgetItem(selection, {tr("Tables")});
    tables->setFlags(tables->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
    findings->setCheckState(0, Qt::Checked);
    for (const auto &t : captured.tables) {
        auto *item=add(tables, t.title);
        // Binary EDID can encode a serial number that text redaction cannot detect.
        if (t.title == QCoreApplication::translate("diagnostictools", "EDID Binary Inspector")) item->setCheckState(0,Qt::Unchecked);
    }
    layout->addWidget(selection);
    auto *includeRaw = new QCheckBox(tr("Include technical details")); includeRaw->setChecked(false);
    auto *redact = new QCheckBox(tr("Redact detected private data")); redact->setChecked(true);
    layout->addWidget(includeRaw); layout->addWidget(redact);
    auto *regenerate = new QPushButton(tr("Regenerate preview (replaces edits)")); layout->addWidget(regenerate);
    auto refresh = [=] {
        auto selected = captured;
        selected.findings.clear(); selected.tables.clear();
        for (int i = 0; i < findings->childCount(); ++i) if (findings->child(i)->checkState(0) == Qt::Checked) selected.findings << captured.findings.at(i);
        for (int i = 0; i < tables->childCount(); ++i) if (tables->child(i)->checkState(0) == Qt::Checked) selected.tables << captured.tables.at(i);
        if (!includeRaw->isChecked()) selected.raw.clear();
        if (redact->isChecked()) diag::redactPrivateFields(selected);
        QString result = tr("Verdict refers to the complete original report; selected sections follow.") + QStringLiteral("\n\n") + selected.toTextWithVerdict(captured);
        if (redact->isChecked()) result = diag::redactPrivateText(result);
        raw->setPlainText(result);
    };
    connect(regenerate, &QPushButton::clicked, &dlg, refresh);
    connect(selection, &QTreeWidget::itemChanged, &dlg, refresh);
    connect(includeRaw, &QCheckBox::toggled, &dlg, refresh);
    connect(redact, &QCheckBox::toggled, &dlg, refresh);
    refresh();
    raw->setReadOnly(false);
    raw->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(raw, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("Copy preview"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [raw] { QApplication::clipboard()->setText(raw->toPlainText()); });
    auto *save = buttons->addButton(tr("Save preview…"), QDialogButtonBox::ActionRole);
    connect(save, &QPushButton::clicked, &dlg, [&dlg, raw] {
        const auto bytes = raw->toPlainText().toUtf8();
        const auto path = QFileDialog::getSaveFileName(&dlg, tr("Save preview"), {}, tr("Text files (*.txt)")); if (path.isEmpty()) return;
        QSaveFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) QMessageBox::warning(&dlg, tr("Save preview"), f.errorString());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.resize(850, 600);
    dlg.exec();
}
