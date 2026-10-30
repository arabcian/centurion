#pragma once
// Home → Boot logo…: replace the Lenovo logo shown at power-on with a picture of one's own.
// Status: health-reader {"op":"boot_logo"} (read-only, silent). Apply / restore:
// legion-firmware-helper {"op":"boot_logo_set" | "boot_logo_reset"} (password every time).
// The picture is always re-encoded here as a baseline JPEG at exactly the firmware's resolution
// (letterboxed on black); see crates/centurion-helpers/src/bootlogo.rs for why.
#include <QDialog>
#include <QImage>
#include <QJsonObject>

class QLabel;
class QPushButton;

/// The JPEG bytes the firmware gets for `src`: `w`×`h`, picture scaled to fit and centred on
/// black, baseline, quality lowered until it fits in `maxBytes`. Empty on failure.
QByteArray encodeBootLogo(const QImage &src, int w, int h, int maxBytes);

class BootLogoDialog : public QDialog {
    Q_OBJECT
public:
    explicit BootLogoDialog(QWidget *parent = nullptr);
    /// The firmware has the logo variables (cheap check, no helper call).
    static bool available();

private:
    void load(const QString &done = {});
    void choose();
    void apply();
    void restore();
    void setBusy(bool busy);

    QJsonObject status_;
    QImage picked_;
    QLabel *info_, *preview_, *msg_;
    QPushButton *choose_, *apply_, *restore_;
};
