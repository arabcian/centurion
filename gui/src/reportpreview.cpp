#include "reportpreview.h"
#include "theme.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QVBoxLayout>

ReportPreview::ReportPreview(const QJsonObject &preview, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Diagnostic Report"));
    resize(860, 560);
    auto *v = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Select files to save. Automatic redaction is best effort (%1 detected values). Review before sharing. "
                                "Long files show only a prefix and are excluded by default. Selected files must still match this preview when saved.")
                                 .arg(preview.value(QStringLiteral("masked")).toInteger()));
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
        it->setData(Qt::UserRole, o.value(QStringLiteral("text")).toString());
        it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
        if (eligible) {
            it->setData(Qt::UserRole + 1, o.value(QStringLiteral("name")).toString());
            it->setData(Qt::UserRole + 2, digest);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(truncated ? Qt::Unchecked : Qt::Checked);
            if (truncated) it->setText(it->text() + tr(" (partial preview; full file if selected)"));
        }
        if (status != QLatin1String("included")) it->setForeground(QColor(QLatin1String(theme::MUTED)));
        list_->addItem(it);
    }
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *cur, QListWidgetItem *) {
        text_->setPlainText(cur ? cur->data(Qt::UserRole).toString() : QString());
    });
    split->addWidget(list_);
    split->addWidget(text_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    v->addWidget(split, 1);
    if (list_->count()) list_->setCurrentRow(0);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save);
    save->setText(tr("Save…"));
    const auto updateSave = [this, save] { save->setEnabled(!selection().isEmpty()); };
    connect(list_, &QListWidget::itemChanged, this, updateSave);
    updateSave();
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

QJsonArray ReportPreview::selection() const {
    QJsonArray result;
    for (int i = 0; i < list_->count(); ++i) {
        const auto *item = list_->item(i);
        if (item->checkState() != Qt::Checked || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        result.append(QJsonObject{{QStringLiteral("name"), item->data(Qt::UserRole + 1).toString()},
                                  {QStringLiteral("sha256"), item->data(Qt::UserRole + 2).toString()}});
    }
    return result;
}

QString ReportPreview::statusText(const QString &status) {
    if (status == QLatin1String("included")) return tr("included");
    if (status == QLatin1String("not present")) return tr("not present");
    if (status == QLatin1String("too large")) return tr("too large, left out");
    return tr("unreadable, left out");
}
