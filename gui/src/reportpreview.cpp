#include "reportpreview.h"
#include "theme.h"
#include "userhelper.h"
#include "workspacestate.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QShortcut>
#include <QTextCursor>
#include <QTextDocument>
#include <QKeySequence>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <limits>

namespace {
constexpr int TextRole = Qt::UserRole;
constexpr int NameRole = Qt::UserRole + 1;
constexpr int DigestRole = Qt::UserRole + 2;
constexpr int BytesRole = Qt::UserRole + 3;
constexpr int TruncatedRole = Qt::UserRole + 4;
}

ReportPreview::ReportPreview(const QJsonObject &preview, const QString &snapshotPath, QWidget *parent)
    : QDialog(parent), snapshotPath_(snapshotPath), pageBytes_(preview.value(QStringLiteral("page_bytes")).toInteger()) {
    setWindowTitle(tr("Diagnostic Report"));
    resize(860, 560);
    auto *v = new QVBoxLayout(this);

    const bool canPage = !snapshotPath_.isEmpty() && pageBytes_ > 0;
    const auto masked = preview.value(QStringLiteral("masked")).toInteger();
    auto *intro = new QLabel(canPage
        ? tr("Select files to save. Automatic redaction is best effort (%1 detected values). Review before sharing. Long files can be reviewed page by page and are excluded by default.").arg(masked)
        : tr("Select files to save. Automatic redaction is best effort (%1 detected values). Review before sharing. "
             "Long files show only a prefix and are excluded by default. Selected files must still match this preview when saved.").arg(masked));
    intro->setTextFormat(Qt::PlainText);
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *split = new QSplitter(Qt::Horizontal);
    list_ = new QListWidget;
    text_ = new QPlainTextEdit;
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    for (const QJsonValue &f : preview.value(QStringLiteral("files")).toArray()) {
        const QJsonObject o = f.toObject();
        const QString status = o.value(QStringLiteral("status")).toString();
        auto *it = new QListWidgetItem(o.value(QStringLiteral("name")).toString() + QStringLiteral("   ") + statusText(status));
        const bool truncated = o.value(QStringLiteral("truncated")).toBool(true);
        const QString digest = o.value(QStringLiteral("sha256")).toString();
        const bool eligible = preview.value(QStringLiteral("preview_schema")).toInt() == 2 && status == QLatin1String("included")
            && QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(digest).hasMatch();
        it->setData(TextRole, o.value(QStringLiteral("text")).toString());
        it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
        if (eligible) {
            it->setData(NameRole, o.value(QStringLiteral("name")).toString());
            it->setData(DigestRole, digest);
            it->setData(BytesRole, o.value(QStringLiteral("bytes")).toInteger());
            it->setData(TruncatedRole, truncated);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(truncated ? Qt::Unchecked : Qt::Checked);
            if (truncated && !canPage) it->setText(it->text() + tr(" (partial preview; full file if selected)"));
        }
        if (status != QLatin1String("included")) it->setForeground(QColor(QLatin1String(theme::MUTED)));
        list_->addItem(it);
    }
    split->addWidget(list_);
    split->addWidget(text_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    workspace::rememberSplitter(split, workspace::key(theme::experimentalEnabled(),
                                                       QStringLiteral("health/report-preview/splitter")));
    v->addWidget(split, 1);
    auto *searchLayout = new QHBoxLayout;
    auto *search = new QLineEdit;
    search->setMaxLength(256);
    search->setPlaceholderText(tr("Find on this page…"));
    search->setAccessibleName(tr("Find on this page"));
    auto *previousMatch = new QPushButton(tr("Previous match"));
    auto *nextMatch = new QPushButton(tr("Next match"));
    previousMatch->setAutoDefault(false);
    nextMatch->setAutoDefault(false);
    auto *searchStatus = new QLabel;
    searchLayout->addWidget(search, 1);
    searchLayout->addWidget(previousMatch);
    searchLayout->addWidget(nextMatch);
    searchLayout->addWidget(searchStatus);
    v->addLayout(searchLayout);

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, search, [search] { search->setFocus(); search->selectAll(); });
    const auto findMatch = [this, search, searchStatus](bool backwards, bool restart) {
        const QString query = search->text();
        if (query.isEmpty()) {
            auto cursor = text_->textCursor();
            cursor.clearSelection();
            text_->setTextCursor(cursor);
            searchStatus->clear();
            return;
        }
        const auto flags = backwards ? QTextDocument::FindFlags(QTextDocument::FindBackward) : QTextDocument::FindFlags{};
        auto start = text_->textCursor();
        if (start.hasSelection() && !restart)
            start.setPosition(backwards ? start.selectionStart() : start.selectionEnd());
        if (restart) start.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        auto match = text_->document()->find(query, start, flags);
        if (match.isNull() && !restart) {
            start.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
            match = text_->document()->find(query, start, flags);
        }
        if (match.isNull()) {
            auto cursor = text_->textCursor();
            cursor.clearSelection();
            text_->setTextCursor(cursor);
            searchStatus->setText(tr("No match on this page"));
            return;
        }
        searchStatus->clear();
        text_->setTextCursor(match);
        text_->ensureCursorVisible();
    };
    const auto updateSearch = [search, previousMatch, nextMatch, findMatch] {
        const bool enabled = !search->text().isEmpty();
        previousMatch->setEnabled(enabled);
        nextMatch->setEnabled(enabled);
        findMatch(false, true);
    };
    connect(search, &QLineEdit::textChanged, this, updateSearch);
    connect(text_, &QPlainTextEdit::textChanged, this, updateSearch);
    auto *findNextShortcut = new QShortcut(search);
    findNextShortcut->setContext(Qt::WidgetShortcut);
    findNextShortcut->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    connect(findNextShortcut, &QShortcut::activated, this, [findMatch] { findMatch(false, false); });
    connect(previousMatch, &QPushButton::clicked, this, [findMatch] { findMatch(true, false); });
    connect(nextMatch, &QPushButton::clicked, this, [findMatch] { findMatch(false, false); });
    updateSearch();
    pager_ = new QWidget;
    auto *navigation = new QHBoxLayout(pager_);
    navigation->setContentsMargins(0, 0, 0, 0);
    pageStatus_ = new QLabel;
    pageStatus_->setTextFormat(Qt::PlainText);
    pageStatus_->setWordWrap(true);
    previousPage_ = new QPushButton(tr("Previous page"));
    nextPage_ = new QPushButton(tr("Next page"));
    jumpPage_ = new QPushButton(tr("Go to page…"));
    for (auto *button : {previousPage_, nextPage_, jumpPage_}) button->setAutoDefault(false);
    navigation->addWidget(pageStatus_, 1);
    navigation->addWidget(previousPage_);
    navigation->addWidget(nextPage_);
    navigation->addWidget(jumpPage_);
    v->addWidget(pager_);
    connect(previousPage_, &QPushButton::clicked, this, [this] { showPage(page_ - 1); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { showPage(page_ + 1); });
    connect(jumpPage_, &QPushButton::clicked, this, [this] {
        if (loadingPage_ || pageCount_ <= 1 || pageCount_ > std::numeric_limits<int>::max()) return;
        const auto generation = generation_;
        bool accepted = false;
        const int target = QInputDialog::getInt(this, tr("Go to page"), tr("Page number"),
            int(page_ + 1), 1, int(pageCount_), 1, &accepted);
        if (accepted && generation == generation_) showPage(target - 1);
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item, QListWidgetItem *) {
        selectFile(item);
    });
    if (list_->count()) list_->setCurrentRow(0);
    else selectFile(nullptr);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setText(tr("Save…"));
    auto *selectionBar = new QHBoxLayout;
    auto *selected = new QLabel;
    auto *selectComplete = new QPushButton(tr("Select complete previews"));
    auto *clear = new QPushButton(tr("Clear file selection"));
    selectComplete->setAutoDefault(false);
    clear->setAutoDefault(false);
    selectionBar->addWidget(selected, 1);
    selectionBar->addWidget(selectComplete);
    selectionBar->addWidget(clear);
    v->addLayout(selectionBar);

    const auto updateSelection = [this, save, selected, selectComplete, clear] {
        int checked = 0;
        bool completeAvailable = false;
        for (int i = 0; i < list_->count(); ++i) {
            const auto *item = list_->item(i);
            if (!item->flags().testFlag(Qt::ItemIsUserCheckable)) continue;
            if (item->checkState() == Qt::Checked) ++checked;
            else if (!item->data(TruncatedRole).toBool()) completeAvailable = true;
        }
        selected->setText(tr("%1 selected").arg(checked));
        save->setEnabled(checked > 0);
        clear->setEnabled(checked > 0);
        selectComplete->setEnabled(completeAvailable);
    };
    const auto setSelection = [this, updateSelection](bool includeComplete) {
        const QSignalBlocker blocker(list_);
        for (int i = 0; i < list_->count(); ++i) {
            auto *item = list_->item(i);
            if (!item->flags().testFlag(Qt::ItemIsUserCheckable)) continue;
            if (!includeComplete) item->setCheckState(Qt::Unchecked);
            else if (!item->data(TruncatedRole).toBool()) item->setCheckState(Qt::Checked);
        }
        updateSelection();
    };
    connect(selectComplete, &QPushButton::clicked, this, [setSelection] { setSelection(true); });
    connect(clear, &QPushButton::clicked, this, [setSelection] { setSelection(false); });
    connect(list_, &QListWidget::itemChanged, this, updateSelection);
    updateSelection();
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

void ReportPreview::selectFile(QListWidgetItem *item) {
    ++generation_;
    loadingPage_ = false;
    page_ = 0;
    const auto bytes = item ? item->data(BytesRole).toLongLong() : 0;
    pageCount_ = !snapshotPath_.isEmpty() && pageBytes_ > 0 && bytes > 0
        ? 1 + (bytes - 1) / pageBytes_ : 1;
    text_->setPlainText(item ? item->data(TextRole).toString() : QString());
    updatePageControls();
}

void ReportPreview::updatePageControls() {
    pager_->setVisible(pageCount_ > 1);
    previousPage_->setEnabled(!loadingPage_ && page_ > 0);
    nextPage_->setEnabled(!loadingPage_ && page_ + 1 < pageCount_);
    jumpPage_->setEnabled(!loadingPage_ && pageCount_ > 1 && pageCount_ <= std::numeric_limits<int>::max());
    pageStatus_->setText(loadingPage_ ? tr("Reading preview page…") : tr("Page %1 of %2").arg(page_ + 1).arg(pageCount_));
}

void ReportPreview::showPage(qint64 page) {
    const auto *item = list_->currentItem();
    if (!item || loadingPage_ || page < 0 || page >= pageCount_ || page == page_) return;
    if (page == 0) {
        selectFile(list_->currentItem());
        return;
    }
    const QJsonObject file{{QStringLiteral("name"), item->data(NameRole).toString()},
                           {QStringLiteral("sha256"), item->data(DigestRole).toString()}};
    loadingPage_ = true;
    const auto generation = ++generation_;
    updatePageControls();
    userhelper::run({{QStringLiteral("op"), QStringLiteral("diag_report_page")},
                     {QStringLiteral("snapshot"), snapshotPath_},
                     {QStringLiteral("preview_schema"), 2},
                     {QStringLiteral("selected"), QJsonArray{file}},
                     {QStringLiteral("page"), page}}, this, [this, page, generation](const QJsonObject &reply) {
        if (generation != generation_) return;
        loadingPage_ = false;
        if (reply.value(QStringLiteral("ok")).toBool()
            && reply.value(QStringLiteral("page")).toInteger(-1) == page
            && reply.value(QStringLiteral("page_count")).toInteger() == pageCount_
            && reply.value(QStringLiteral("text")).isString()) {
            page_ = page;
            text_->setPlainText(reply.value(QStringLiteral("text")).toString());
            updatePageControls();
            return;
        }
        updatePageControls();
        const auto error = reply.value(QStringLiteral("error")).toString();
        pageStatus_->setText(error.isEmpty() ? tr("Could not read the preview page.")
                                             : tr("Could not read the preview page: %1").arg(error));
    });
}

QJsonArray ReportPreview::selection() const {
    QJsonArray result;
    for (int i = 0; i < list_->count(); ++i) {
        const auto *item = list_->item(i);
        if (item->checkState() != Qt::Checked || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        result.append(QJsonObject{{QStringLiteral("name"), item->data(NameRole).toString()},
                                  {QStringLiteral("sha256"), item->data(DigestRole).toString()}});
    }
    return result;
}

QString ReportPreview::statusText(const QString &status) {
    if (status == QLatin1String("included")) return tr("included");
    if (status == QLatin1String("not present")) return tr("not present");
    if (status == QLatin1String("too large")) return tr("too large, left out");
    return tr("unreadable, left out");
}
