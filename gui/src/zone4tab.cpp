#include "zone4tab.h"
#include "lighting.h"
#include "theme.h"
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

static QString storePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/centurion/zone4.ini");
}

Zone4Tab::Zone4Tab(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(8);

    auto *box = new QGroupBox("Keyboard backlight (4 zones)");
    auto *g = new QGridLayout(box);
    g->setContentsMargins(10, 6, 10, 8);
    g->setHorizontalSpacing(10);
    g->setVerticalSpacing(6);
    auto muted = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setStyleSheet(QStringLiteral("color:%1").arg(theme::MUTED));
        return l;
    };
    int row = 0;

    effect_ = new QComboBox;
    effect_->addItem("Static", "static");
    effect_->addItem("Breath", "breath");
    effect_->addItem("Wave", "wave");
    effect_->addItem("Smooth", "smooth");
    effect_->setToolTip("Static and Breath use the four zone colours below.\n"
                        "Wave and Smooth cycle through the keyboard's own colours.");
    g->addWidget(muted("Effect"), row, 0);
    g->addWidget(effect_, row++, 1);

    brightness_ = new QComboBox;
    brightness_->addItem("Low", 1);
    brightness_->addItem("High", 2);
    g->addWidget(muted("Brightness"), row, 0);
    g->addWidget(brightness_, row++, 1);

    speed_ = new QComboBox;
    speed_->addItem("Slowest", 1);
    speed_->addItem("Slow", 2);
    speed_->addItem("Fast", 3);
    speed_->addItem("Fastest", 4);
    speedLabel_ = muted("Speed");
    g->addWidget(speedLabel_, row, 0);
    g->addWidget(speed_, row++, 1);

    direction_ = new QComboBox;
    direction_->addItem("Left to right", false);
    direction_->addItem("Right to left", true);
    directionLabel_ = muted("Direction");
    g->addWidget(directionLabel_, row, 0);
    g->addWidget(direction_, row++, 1);

    zonesLabel_ = muted("Zone colours");
    auto *zh = new QHBoxLayout;
    zh->setSpacing(6);
    for (int i = 0; i < 4; ++i) {
        zone_[i] = new QPushButton;
        zone_[i]->setMinimumSize(96, 44);
        zone_[i]->setToolTip(QStringLiteral("Zone %1 (left to right) — click to pick its colour.").arg(i + 1));
        connect(zone_[i], &QPushButton::clicked, this, [this, i] {
            const QColor c = QColorDialog::getColor(colors_[i], this, QStringLiteral("Zone %1 colour").arg(i + 1));
            if (!c.isValid()) return;
            colors_[i] = c;
            paintZone(i);
        });
        zh->addWidget(zone_[i], 1);
    }
    g->addWidget(zonesLabel_, row, 0);
    g->addLayout(zh, row++, 1);
    g->setColumnStretch(1, 1);
    v->addWidget(box);

    auto *note = muted("This keyboard cannot be read back: the values shown are the ones last applied from here. "
                       "Fn+Space on the keyboard still cycles the firmware's own presets.");
    note->setWordWrap(true);
    v->addWidget(note);

    auto *bh = new QHBoxLayout;
    status_ = new QLabel;
    status_->setWordWrap(true);
    off_ = new QPushButton("Lights off");
    apply_ = new QPushButton("Apply");
    apply_->setDefault(true);
    bh->addWidget(status_, 1);
    bh->addWidget(off_);
    bh->addWidget(apply_);
    v->addLayout(bh);
    v->addStretch(1);

    connect(effect_, &QComboBox::currentIndexChanged, this, [this] { syncEnabled(); });
    connect(apply_, &QPushButton::clicked, this, [this] { apply(false); });
    connect(off_, &QPushButton::clicked, this, [this] { apply(true); });

    load();
    for (int i = 0; i < 4; ++i) paintZone(i);
    syncEnabled();
}

void Zone4Tab::paintZone(int i) {
    const QColor &c = colors_[i];
    // Legible caption on any colour.
    const QColor fg = (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000 > 140 ? QColor(Qt::black) : QColor(Qt::white);
    zone_[i]->setText(c.name(QColor::HexRgb).toUpper());
    zone_[i]->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid %3; border-radius:6px; }")
                                .arg(c.name(QColor::HexRgb), fg.name(QColor::HexRgb), QLatin1String(theme::BG3)));
}

void Zone4Tab::syncEnabled() {
    const QString e = effect_->currentData().toString();
    const bool colors = e == QLatin1String("static") || e == QLatin1String("breath");
    const bool speed = e != QLatin1String("static");
    const bool dir = e == QLatin1String("wave");
    speedLabel_->setVisible(speed);
    speed_->setVisible(speed);
    directionLabel_->setVisible(dir);
    direction_->setVisible(dir);
    zonesLabel_->setVisible(colors);
    for (QPushButton *b : zone_) b->setVisible(colors);
    apply_->setEnabled(!busy_);
    off_->setEnabled(!busy_);
}

void Zone4Tab::load() {
    QSettings st(storePath(), QSettings::IniFormat);
    auto pick = [](QComboBox *c, const QVariant &v) { if (const int i = c->findData(v); i >= 0) c->setCurrentIndex(i); };
    pick(effect_, st.value(QStringLiteral("zone4/effect"), QStringLiteral("static")).toString());
    pick(brightness_, st.value(QStringLiteral("zone4/brightness"), 2).toInt());
    pick(speed_, st.value(QStringLiteral("zone4/speed"), 2).toInt());
    pick(direction_, st.value(QStringLiteral("zone4/right_to_left"), false).toBool());
    for (int i = 0; i < 4; ++i) {
        const QColor c(st.value(QStringLiteral("zone4/color%1").arg(i), QStringLiteral("#ffffff")).toString());
        if (c.isValid()) colors_[i] = c;
    }
}

void Zone4Tab::save() const {
    QDir().mkpath(QFileInfo(storePath()).absolutePath());
    QSettings st(storePath(), QSettings::IniFormat);
    st.setValue(QStringLiteral("zone4/effect"), effect_->currentData().toString());
    st.setValue(QStringLiteral("zone4/brightness"), brightness_->currentData().toInt());
    st.setValue(QStringLiteral("zone4/speed"), speed_->currentData().toInt());
    st.setValue(QStringLiteral("zone4/right_to_left"), direction_->currentData().toBool());
    for (int i = 0; i < 4; ++i) st.setValue(QStringLiteral("zone4/color%1").arg(i), colors_[i].name(QColor::HexRgb));
}

void Zone4Tab::apply(bool off) {
    if (busy_) return;
    QJsonObject req{{"op", "z4_set"}};
    if (off) {
        req["effect"] = "off";
    } else {
        QJsonArray cols;
        for (const QColor &c : colors_) cols.append(c.name(QColor::HexRgb).mid(1));
        req["effect"] = effect_->currentData().toString();
        req["brightness"] = brightness_->currentData().toInt();
        req["speed"] = speed_->currentData().toInt();
        req["right_to_left"] = direction_->currentData().toBool();
        req["colors"] = cols;
    }
    busy_ = true;
    syncEnabled();
    status_->setText(QStringLiteral("<span style='color:%1'>Applying…</span>").arg(theme::MUTED));
    lighting::run(req, this, [this, off](const privileged::Result &r) {
        busy_ = false;
        syncEnabled();
        if (!r.ok()) {
            status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::DANGER, r.message().toHtmlEscaped()));
            return;
        }
        if (!off) save();
        status_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(theme::OK, off ? QStringLiteral("Lights off") : QStringLiteral("Applied")));
    });
}
