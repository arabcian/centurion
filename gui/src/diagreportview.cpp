#include "diagreportview.h"

#include "sysadmin.h"
#include "theme.h"
#include "toolcatalog.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
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
        tree->setColumnCount(qMax(1, int(t.headers.size())));
        tree->setHeaderLabels(t.headers);
        tree->setHeaderHidden(t.headers.isEmpty());
        tree->setRootIsDecorated(false);
        tree->setAlternatingRowColors(true);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree->setUniformRowHeights(true);
        tree->setWordWrap(false);
        for (int i = 0; i < t.rows.size(); ++i) {
            auto *it = new QTreeWidgetItem(t.rows[i]);
            for (int c = 0; c < t.rows[i].size(); ++c) it->setToolTip(c, t.rows[i][c]);
            const Level l = i < t.levels.size() ? t.levels[i] : Level::Info;
            if (l == Level::Attention || l == Level::Review || l == Level::Ok) it->setForeground(0, QColor(color(l)));
            tree->addTopLevelItem(it);
        }
        for (int c = 0; c < tree->columnCount(); ++c) tree->resizeColumnToContents(c);
        tree->header()->setStretchLastSection(true);
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
    top->addWidget(verdict_);
    top->addWidget(headline_, 1);
    copy_ = new QPushButton(tr("Copy report"));
    save_ = new QPushButton(tr("Save report…"));
    copy_->setEnabled(false);
    save_->setEnabled(false);
    top->addWidget(copy_);
    top->addWidget(save_);
    v->addLayout(top);
    about_ = text(QString(), "muted");
    v->addWidget(about_);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    body_ = new QWidget;
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(0, 2, 6, 6);
    bodyLayout_->setSpacing(8);
    scroll->setWidget(body_);
    v->addWidget(scroll, 1);

    connect(copy_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(report_.toText()); });
    connect(save_, &QPushButton::clicked, this, [this] {
        const QString name = QStringLiteral("centurion-%1-%2.txt")
                                 .arg(report_.code.toLower(), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
        const QString path = QFileDialog::getSaveFileName(this, tr("Save report"), QDir::homePath() + QLatin1Char('/') + name,
                                                          tr("Text files (*.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(report_.toText().toUtf8()) < 0)
            headline_->setText(tr("Could not save %1: %2").arg(path, f.errorString()));
    });
    setMessage(QString());
}

void DiagReportView::clearBody() {
    while (QLayoutItem *item = bodyLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void DiagReportView::setMessage(const QString &s) {
    clearBody();
    verdict_->clear();
    verdict_->setVisible(false);
    headline_->setText(s);
    about_->setVisible(false);
    copy_->setEnabled(false);
    save_->setEnabled(false);
    bodyLayout_->addStretch();
}

void DiagReportView::setReport(const diag::Report &r) {
    report_ = r;
    clearBody();
    const Level v = r.verdict();
    verdict_->setText(diag::levelName(v));
    verdict_->setStyleSheet(QStringLiteral("font-weight:700;color:%1").arg(color(v)));
    verdict_->setVisible(true);
    headline_->setText(QStringLiteral("· ") + r.headline());
    about_->setText(r.about);
    about_->setVisible(!r.about.isEmpty());
    copy_->setEnabled(true);
    save_->setEnabled(true);

    // Most serious first, the order within a level as the diagnostic wrote it.
    QList<diag::Finding> findings = r.findings;
    std::stable_sort(findings.begin(), findings.end(), [](const diag::Finding &a, const diag::Finding &b) { return a.level > b.level; });
    for (const diag::Finding &f : findings) bodyLayout_->addWidget(findingCard(f, this));
    for (const diag::Table &t : r.tables) bodyLayout_->addWidget(tableWidget(t));
    if (!r.limits.isEmpty()) {
        auto *lim = text(tr("Limits of this check:") + QStringLiteral("\n• ") + r.limits.join(QStringLiteral("\n• ")), "muted");
        bodyLayout_->addWidget(lim);
    }
    if (!r.raw.trimmed().isEmpty()) {
        auto *toggle = new QPushButton(tr("Show technical details"));
        toggle->setCheckable(true);
        auto *raw = new QPlainTextEdit(r.raw.trimmed());
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
        bodyLayout_->addWidget(toggle, 0, Qt::AlignLeft);
        bodyLayout_->addWidget(raw);
    }
    bodyLayout_->addStretch();
}
