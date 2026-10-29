#include "reportpreview.h"
#include "theme.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

ReportPreview::ReportPreview(const QJsonObject &preview, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Diagnostic report"));
    resize(860, 560);
    auto *v = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("These are the files of the report, as they will be saved. Serial numbers, UUIDs, "
                                "MAC and IP addresses have already been replaced in the text (%1 value(s) so far). "
                                "Nothing is sent anywhere: the archive is written only to the place you choose.")
                                 .arg(preview.value(QStringLiteral("masked")).toInteger()));
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
        it->setData(Qt::UserRole, o.value(QStringLiteral("text")).toString());
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
    buttons->button(QDialogButtonBox::Save)->setText(tr("Save…"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);
}

QString ReportPreview::statusText(const QString &status) {
    if (status == QLatin1String("included")) return tr("included");
    if (status == QLatin1String("not present")) return tr("not present");
    if (status == QLatin1String("too large")) return tr("too large, left out");
    return tr("unreadable, left out");
}
