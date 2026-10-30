#include "bootlogodialog.h"
#include "privileged.h"
#include "theme.h"
#include <QBuffer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr const char *DESC_VAR = "/sys/firmware/efi/efivars/LBLDESP-871455d0-5576-4fb8-9865-af0824463b9e";
constexpr int MAX_LOGO = 4 << 20;  // bootlogo::MAX_LOGO

// The picture scaled to fit w×h and centred on black: what the firmware will show.
QImage letterbox(const QImage &src, int w, int h) {
    QImage canvas(w, h, QImage::Format_RGB888);
    canvas.fill(Qt::black);
    const QImage fit = src.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter(&canvas).drawImage((w - fit.width()) / 2, (h - fit.height()) / 2, fit);
    return canvas;
}
}

QByteArray encodeBootLogo(const QImage &src, int w, int h, int maxBytes) {
    if (src.isNull() || w <= 0 || h <= 0) return {};
    const QImage canvas = letterbox(src, w, h);
    for (int q : {90, 80, 70, 60}) {
        QByteArray out;
        QBuffer buf(&out);
        buf.open(QIODevice::WriteOnly);
        QImageWriter wr(&buf, "jpeg");
        wr.setQuality(q);
        wr.setProgressiveScanWrite(false);
        if (!wr.write(canvas)) return {};
        if (out.size() <= maxBytes) return out;
    }
    return {};
}

bool BootLogoDialog::available() { return QFileInfo::exists(QString::fromLatin1(DESC_VAR)); }

BootLogoDialog::BootLogoDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Boot logo"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(620, 560);
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Replace the Lenovo logo shown at power-on. The picture is saved on the EFI system "
                                "partition and the firmware shows it from the next boot. Writing asks for the "
                                "administrator password every time."));
    intro->setWordWrap(true);
    v->addWidget(intro);

    info_ = new QLabel;
    info_->setWordWrap(true);
    info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(info_);

    preview_ = new QLabel;
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumHeight(300);
    preview_->setStyleSheet(QStringLiteral("background:#000; border:1px solid %1;").arg(theme::MUTED));
    v->addWidget(preview_, 1);

    msg_ = new QLabel;
    msg_->setWordWrap(true);
    msg_->setTextFormat(Qt::RichText);
    v->addWidget(msg_);

    auto *h = new QHBoxLayout;
    choose_ = new QPushButton(tr("Choose picture…"));
    apply_ = new QPushButton(tr("Apply"));
    restore_ = new QPushButton(tr("Restore Lenovo logo"));
    auto *close = new QPushButton(tr("Close"));
    apply_->setEnabled(false);
    restore_->setEnabled(false);
    choose_->setEnabled(false);
    h->addWidget(choose_);
    h->addWidget(apply_);
    h->addWidget(restore_);
    h->addStretch(1);
    h->addWidget(close);
    v->addLayout(h);
    connect(choose_, &QPushButton::clicked, this, &BootLogoDialog::choose);
    connect(apply_, &QPushButton::clicked, this, &BootLogoDialog::apply);
    connect(restore_, &QPushButton::clicked, this, &BootLogoDialog::restore);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    load();
}

void BootLogoDialog::setBusy(bool busy) {
    const bool ok = status_.value("supported").toBool() && status_.value("jpeg").toBool() && status_.contains("esp");
    choose_->setEnabled(!busy && ok);
    apply_->setEnabled(!busy && ok && !picked_.isNull());
    restore_->setEnabled(!busy && status_.value("supported").toBool()
                         && (status_.value("enabled").toBool() || !status_.value("files").toArray().isEmpty()));
}

void BootLogoDialog::load(const QString &done) {
    msg_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED, tr("Reading…")));
    setBusy(true);
    privileged::run(privileged::helperPath(QStringLiteral("health-reader")), QJsonObject{{"op", "boot_logo"}}, this,
                    [this, done](const privileged::Result &r) {
        msg_->setText(done.isEmpty() ? QString() : QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::OK, done.toHtmlEscaped()));
        status_ = r.ok() ? r.json : QJsonObject{};
        if (!r.ok()) {
            info_->setText(tr("Could not read the logo settings: %1").arg(r.message()));
        } else if (!status_.value("supported").toBool()) {
            info_->setText(tr("This firmware has no custom boot logo support."));
        } else {
            QStringList fmts;
            for (const QJsonValue &f : status_.value("formats").toArray()) fmts << f.toString().toUpper();
            QString t = tr("Custom logo: %1 · picture size %2×%3 · the firmware lists %4")
                            .arg(status_.value("enabled").toBool() ? tr("on") : tr("off"))
                            .arg(status_.value("width").toInt()).arg(status_.value("height").toInt())
                            .arg(fmts.join(QStringLiteral(", ")));
            if (status_.contains("esp")) {
                const QJsonArray files = status_.value("files").toArray();
                t += '\n' + tr("EFI partition: %1").arg(status_.value("esp").toString());
                for (const QJsonValue &f : files) t += '\n' + tr("Logo file: %1").arg(f.toString());
            } else {
                t += '\n' + status_.value("esp_error").toString();
            }
            if (!status_.value("jpeg").toBool()) t += '\n' + tr("The firmware does not list JPEG; Centurion only writes JPEG logos.");
            info_->setText(t);
        }
        setBusy(false);
    });
}

void BootLogoDialog::choose() {
    QStringList filters;
    for (const QByteArray &f : QImageReader::supportedImageFormats()) filters << QStringLiteral("*.") + QString::fromLatin1(f);
    const QString path = QFileDialog::getOpenFileName(this, tr("Boot logo picture"), QString(),
                                                      tr("Images (%1)").arg(filters.join(' ')));
    if (path.isEmpty()) return;
    QImageReader rd(path);
    rd.setAutoTransform(true);
    const QImage img = rd.read();
    if (img.isNull()) { msg_->setText(tr("Cannot read that picture: %1").arg(rd.errorString().toHtmlEscaped())); return; }
    picked_ = img;
    const QImage canvas = letterbox(img, status_.value("width").toInt(), status_.value("height").toInt());
    preview_->setPixmap(QPixmap::fromImage(canvas.scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    msg_->setText(tr("%1 (%2×%3) — scaled to fit, black borders.").arg(QFileInfo(path).fileName().toHtmlEscaped())
                      .arg(img.width()).arg(img.height()));
    setBusy(false);
}

void BootLogoDialog::apply() {
    const int w = status_.value("width").toInt(), h = status_.value("height").toInt();
    const QByteArray jpeg = encodeBootLogo(picked_, w, h, MAX_LOGO);
    if (jpeg.isEmpty()) { msg_->setText(tr("Could not encode the picture as JPEG.")); return; }
    if (QMessageBox::question(this, tr("Boot logo"),
            tr("Write this picture as the boot logo?\n\nIt is saved as EFI/Lenovo/Logo/mylogo_%1x%2.jpg on the EFI "
               "partition and the firmware's logo settings are switched on. It shows from the next boot; "
               "\"Restore Lenovo logo\" undoes it.").arg(w).arg(h)) != QMessageBox::Yes) return;
    setBusy(true);
    msg_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::MUTED, tr("Writing…")));
    privileged::run(privileged::helperPath(privileged::FIRMWARE_HELPER),
                    QJsonObject{{"op", "boot_logo_set"}, {"jpeg_hex", QString::fromLatin1(jpeg.toHex())}}, this,
                    [this](const privileged::Result &r) {
        if (!r.ok()) {
            QMessageBox::warning(this, tr("Boot logo"), tr("The boot logo was not changed:\n\n%1").arg(r.message())
                                 + (r.json.value("state").toString() == QLatin1String("unknown")
                                        ? tr("\n\nA step could not be undone; \"Restore Lenovo logo\" puts the default back.") : QString()));
        }
        load(r.ok() ? tr("Boot logo set — it shows from the next boot.") : QString());
    }, privileged::FIRMWARE_TIMEOUT_MS);
}

void BootLogoDialog::restore() {
    if (QMessageBox::question(this, tr("Boot logo"), tr("Switch back to the Lenovo logo and delete the custom picture from the EFI partition?"))
        != QMessageBox::Yes) return;
    setBusy(true);
    privileged::run(privileged::helperPath(privileged::FIRMWARE_HELPER), QJsonObject{{"op", "boot_logo_reset"}}, this,
                    [this](const privileged::Result &r) {
        if (!r.ok()) QMessageBox::warning(this, tr("Boot logo"), tr("Could not restore the Lenovo logo:\n\n%1").arg(r.message()));
        load(r.ok() ? tr("Lenovo logo restored — from the next boot.") : QString());
    }, privileged::FIRMWARE_TIMEOUT_MS);
}
