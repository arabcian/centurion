#include "reportpreview.h"
#include "theme.h"
#include "userhelper.h"
#include "workspacestate.h"

#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QShortcut>
#include <QTextCursor>
#include <QTextDocument>
#include <QKeySequence>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStringList>
#include <QSignalBlocker>
#include <QHash>
#include <QVBoxLayout>
#include <limits>

namespace {
constexpr int TextRole = Qt::UserRole;
constexpr int NameRole = Qt::UserRole + 1;
constexpr int DigestRole = Qt::UserRole + 2;
constexpr int BytesRole = Qt::UserRole + 3;
constexpr int TruncatedRole = Qt::UserRole + 4;
constexpr int PageDigestsRole = Qt::UserRole + 5;
constexpr int PageCountRole = Qt::UserRole + 6;
constexpr int ShownPageCountRole = Qt::UserRole + 7;
constexpr int AvailableFieldsRole = Qt::UserRole + 8;
constexpr int SelectedFieldsRole = Qt::UserRole + 9;
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
    const auto files = preview.value(QStringLiteral("files")).toArray();
    QHash<QString, int> nameCounts;
    for (const auto &file : files) ++nameCounts[file.toObject().value(QStringLiteral("name")).toString()];
    for (const auto &f : files) {
        const auto o = f.toObject();
        const auto name = o.value(QStringLiteral("name")).toString();
        auto *item = new QListWidgetItem;
        const bool eligible = preview.value(QStringLiteral("preview_schema")).toInt() == 2 && nameCounts.value(name) == 1;
        if (loadFilePreview(item, o, eligible) && canPage) {
            QStringList fields;
            for (const auto &field : o.value(QStringLiteral("fields")).toArray()) fields << field.toString();
            if (!fields.contains(QString()) && fields.size() > 1) item->setData(AvailableFieldsRole, fields);
        }
        list_->addItem(item);
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
    unopenedPage_ = new QPushButton(tr("Next unopened page"));
    retryPage_ = new QPushButton(tr("Retry page"));
    for (auto *button : {previousPage_, nextPage_, jumpPage_, unopenedPage_, retryPage_}) button->setAutoDefault(false);
    navigation->addWidget(pageStatus_, 1);
    navigation->addWidget(retryPage_);
    navigation->addWidget(previousPage_);
    navigation->addWidget(nextPage_);
    navigation->addWidget(jumpPage_);
    navigation->addWidget(unopenedPage_);
    v->addWidget(pager_);
    connect(previousPage_, &QPushButton::clicked, this, [this] { showPage(page_ - 1); });
    connect(nextPage_, &QPushButton::clicked, this, [this] { showPage(page_ + 1); });
    connect(unopenedPage_, &QPushButton::clicked, this, [this] { showPage(nextUnopenedPage()); });
    connect(retryPage_, &QPushButton::clicked, this, [this] { showPage(failedPage_); });
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
    auto *fields = new QPushButton(tr("Choose fields…"));
    fields->setAutoDefault(false);
    fields->setToolTip(tr("Available for system details and structured records. Other files can be included or excluded as a whole."));
    v->addWidget(fields);
    const auto updateFields = [this, fields] {
        const auto *item = list_->currentItem();
        fields->setEnabled(item && !item->data(AvailableFieldsRole).toStringList().isEmpty());
    };
    connect(list_, &QListWidget::currentItemChanged, this, updateFields);
    connect(fields, &QPushButton::clicked, this, &ReportPreview::editFields);
    updateFields();
    if (list_->count()) list_->setCurrentRow(0);
    else selectFile(nullptr);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setText(tr("Save…"));
    auto *selectionBar = new QHBoxLayout;
    auto *selected = new QLabel;
    selected->setWordWrap(true);
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
        int incomplete = 0;
        bool completeAvailable = false;
        for (int i = 0; i < list_->count(); ++i) {
            const auto *item = list_->item(i);
            if (!item->flags().testFlag(Qt::ItemIsUserCheckable)) continue;
            if (item->checkState() == Qt::Checked) {
                ++checked;
                const auto pages = item->data(PageCountRole).toLongLong();
                if (item->data(TruncatedRole).toBool()
                    && (pages == 1 || item->data(ShownPageCountRole).toLongLong() < pages)) ++incomplete;
            } else if (!item->data(TruncatedRole).toBool()) {
                completeAvailable = true;
            }
        }
        selected->setText(incomplete ? tr("%1 selected · %2 include text you have not opened").arg(checked).arg(incomplete)
                                     : tr("%1 selected").arg(checked));
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

bool ReportPreview::loadFilePreview(QListWidgetItem *it, const QJsonObject &o, bool candidate) const {
    static const QRegularExpression digestPattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    const bool canPage = !snapshotPath_.isEmpty() && pageBytes_ > 0;
    const auto name = o.value(QStringLiteral("name")).toString();
    const auto status = o.value(QStringLiteral("status")).toString();
    it->setText(name + QStringLiteral("   ") + statusText(status));
    const auto truncatedValue = o.value(QStringLiteral("truncated"));
    const bool truncated = truncatedValue.toBool(true);
    const QString digest = o.value(QStringLiteral("sha256")).toString();
    const auto shown = o.value(QStringLiteral("text"));
    const QByteArray shownBytes = shown.toString().toUtf8();
    const auto bytes = o.value(QStringLiteral("bytes")).toInteger(-1);
    const auto digestList = o.value(QStringLiteral("page_sha256"));
    const qint64 pageCount = canPage && bytes > 0 ? 1 + (bytes - 1) / pageBytes_ : 1;
    QStringList pageDigests;
    bool validDigests = true;
    if (canPage && truncated && !digestList.isUndefined()) {
        const auto values = digestList.toArray();
        validDigests = values.size() == pageCount && !values.isEmpty();
        for (const auto &value : values) {
            const auto hash = value.toString();
            if (!digestPattern.match(hash).hasMatch()) validDigests = false;
            pageDigests.append(hash);
        }
        validDigests = validDigests && QCryptographicHash::hash(shownBytes, QCryptographicHash::Sha256).toHex()
            == pageDigests.constFirst().toLatin1();
    }
    const bool matchesPreview = truncated
        ? !shownBytes.isEmpty() && bytes > shownBytes.size()
            && (!canPage || (pageCount > 1 && shownBytes.size() >= pageBytes_ - 3 && shownBytes.size() <= pageBytes_))
        : bytes == shownBytes.size() && QCryptographicHash::hash(shownBytes, QCryptographicHash::Sha256).toHex() == digest.toLatin1();
    const bool eligible = candidate && status == QLatin1String("included")
        && !name.isEmpty() && shown.isString() && truncatedValue.isBool()
        && digestPattern.match(digest).hasMatch() && matchesPreview && validDigests;
    it->setData(TextRole, shown.toString());
    it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
    if (eligible) {
        it->setData(NameRole, name);
        it->setData(DigestRole, digest);
        it->setData(BytesRole, bytes);
        it->setData(TruncatedRole, truncated);
        it->setData(PageDigestsRole, pageDigests);
        it->setData(PageCountRole, pageCount);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(truncated ? Qt::Unchecked : Qt::Checked);
        if (truncated && !canPage) it->setText(it->text() + tr(" (partial preview; full file if selected)"));
    }
    if (status == QLatin1String("included") && !eligible)
        it->setText(it->text() + tr(" (invalid preview metadata)"));
    if (status != QLatin1String("included")) it->setForeground(QColor(QLatin1String(theme::MUTED)));
    return eligible;
}

QJsonObject ReportPreview::fileSelection(const QListWidgetItem *item) const {
    QJsonObject selected{{QStringLiteral("name"), item->data(NameRole).toString()},
                         {QStringLiteral("sha256"), item->data(DigestRole).toString()}};
    const auto fields = item->data(SelectedFieldsRole).toStringList();
    if (!fields.isEmpty()) selected.insert(QStringLiteral("fields"), QJsonArray::fromStringList(fields));
    return selected;
}

void ReportPreview::editFields() {
    auto *item = list_->currentItem();
    if (!item) return;
    const auto available = item->data(AvailableFieldsRole).toStringList();
    if (available.isEmpty()) return;
    const auto current = item->data(SelectedFieldsRole).toStringList();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Choose fields"));
    dialog.resize(560, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *help = new QLabel(tr("Choose which fields to include. A field path applies to every matching record."));
    help->setWordWrap(true);
    layout->addWidget(help);
    auto *list = new QListWidget(&dialog);
    for (const auto &field : available) {
        auto *row = new QListWidgetItem(field, list);
        row->setCheckState(current.isEmpty() || current.contains(field) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *apply = buttons->button(QDialogButtonBox::Ok);
    apply->setText(tr("Update preview"));
    connect(list, &QListWidget::itemChanged, &dialog, [list, apply] {
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->checkState() == Qt::Checked) { apply->setEnabled(true); return; }
        }
        apply->setEnabled(false);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList chosen;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) chosen.append(available.at(i));
    if (chosen.isEmpty()) return;
    if (chosen == available) chosen.clear();
    if (chosen == current) return;

    const auto name = item->data(NameRole).toString();
    QJsonObject selection{{QStringLiteral("name"), name}};
    if (!chosen.isEmpty()) selection.insert(QStringLiteral("fields"), QJsonArray::fromStringList(chosen));
    setEnabled(false);
    userhelper::run({{QStringLiteral("op"), QStringLiteral("diag_report")},
                     {QStringLiteral("snapshot"), snapshotPath_},
                     {QStringLiteral("edit_fields"), selection}}, this,
                    [this, item, name, chosen](const QJsonObject &reply) {
        setEnabled(true);
        const auto file = reply.value(QStringLiteral("file")).toObject();
        QListWidgetItem updated;
        if (!reply.value(QStringLiteral("ok")).toBool() || file.value(QStringLiteral("name")).toString() != name
            || !loadFilePreview(&updated, file, true)) {
            QMessageBox::warning(this, tr("Diagnostic Report"), tr("The field preview could not be updated."));
            return;
        }
        const auto checked = item->checkState();
        const auto fields = item->data(AvailableFieldsRole);
        *item = updated;
        item->setData(AvailableFieldsRole, fields);
        item->setData(SelectedFieldsRole, chosen);
        item->setData(ShownPageCountRole, 0);
        if (!chosen.isEmpty()) item->setText(item->text() + tr(" (selected fields)"));
        if (!item->data(TruncatedRole).toBool()) item->setCheckState(checked);
        reviews_.remove(item);
        selectFile(item);
    });
}

void ReportPreview::selectFile(QListWidgetItem *item) {
    ++generation_;
    loadingPage_ = false;
    failedPage_ = -1;
    page_ = 0;
    pageCount_ = item ? qMax<qint64>(1, item->data(PageCountRole).toLongLong()) : 1;
    text_->setPlainText(item ? item->data(TextRole).toString() : QString());
    const auto review = reviews_.constFind(item);
    const auto lastPage = review == reviews_.cend() ? 0 : review->lastPage;
    if (lastPage > 0) showPage(lastPage);
    else markPageShown(item);
    updatePageControls();
}

void ReportPreview::markPageShown(QListWidgetItem *item) {
    if (!item || pageCount_ <= 1) return;
    auto &review = reviews_[item];
    review.lastPage = page_;
    review.shown.insert(page_);
    item->setData(ShownPageCountRole, qint64(review.shown.size()));
}

qint64 ReportPreview::nextUnopenedPage() const {
    if (pageCount_ <= 1) return -1;
    const auto review = reviews_.constFind(list_->currentItem());
    if (review == reviews_.cend()) return -1;
    for (qint64 page = 0; page < pageCount_; ++page)
        if (!review->shown.contains(page)) return page;
    return -1;
}

void ReportPreview::updatePageControls() {
    pager_->setVisible(pageCount_ > 1);
    previousPage_->setEnabled(!loadingPage_ && page_ > 0);
    nextPage_->setEnabled(!loadingPage_ && page_ + 1 < pageCount_);
    jumpPage_->setEnabled(!loadingPage_ && pageCount_ > 1 && pageCount_ <= std::numeric_limits<int>::max());
    unopenedPage_->setEnabled(!loadingPage_ && nextUnopenedPage() >= 0);
    retryPage_->setVisible(failedPage_ >= 0);
    pageStatus_->setText(loadingPage_ ? tr("Reading preview page…") : tr("Page %1 of %2").arg(page_ + 1).arg(pageCount_));
}

void ReportPreview::showPage(qint64 page) {
    auto *item = list_->currentItem();
    if (!item || loadingPage_ || page < 0 || page >= pageCount_ || page == page_) return;
    failedPage_ = -1;
    if (page == 0) {
        page_ = 0;
        text_->setPlainText(item->data(TextRole).toString());
        markPageShown(item);
        updatePageControls();
        return;
    }
    const auto file = fileSelection(item);
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
        auto *item = list_->currentItem();
        if (!item) { updatePageControls(); return; }
        const auto content = reply.value(QStringLiteral("text"));
        const auto bytes = content.toString().toUtf8();
        const auto nominal = qMin(pageBytes_, item->data(BytesRole).toLongLong() - page * pageBytes_);
        const auto digests = item->data(PageDigestsRole).toStringList();
        if (reply.value(QStringLiteral("ok")).toBool()
            && reply.value(QStringLiteral("page")).toInteger(-1) == page
            && reply.value(QStringLiteral("page_count")).toInteger() == pageCount_
            && content.isString() && !bytes.isEmpty() && bytes.size() >= nominal - 3 && bytes.size() <= nominal + 3
            && (digests.isEmpty() || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == digests.at(page).toLatin1())) {
            page_ = page;
            text_->setPlainText(content.toString());
            markPageShown(item);
            updatePageControls();
            return;
        }
        failedPage_ = page;
        updatePageControls();
        const auto error = reply.value(QStringLiteral("error")).toString();
        pageStatus_->setText(error.isEmpty() ? tr("Could not read page %1.").arg(page + 1)
                                             : tr("Could not read page %1: %2").arg(page + 1).arg(error));
    });
}

QJsonArray ReportPreview::selection() const {
    QJsonArray result;
    for (int i = 0; i < list_->count(); ++i) {
        const auto *item = list_->item(i);
        if (item->checkState() != Qt::Checked || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        result.append(fileSelection(item));
    }
    return result;
}

QString ReportPreview::statusText(const QString &status) {
    if (status == QLatin1String("included")) return tr("included");
    if (status == QLatin1String("not present")) return tr("not present");
    if (status == QLatin1String("too large")) return tr("too large, left out");
    return tr("unreadable, left out");
}
