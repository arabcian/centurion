#pragma once
// Experimental UI primitives. Presentation only: no hardware access and no polling.
// Classic mode never instantiates these widgets.
#include "theme.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QIcon>
#include <QPixmap>
#include <QPolygonF>
#include <QPen>
#include <QPushButton>
#include <QToolButton>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <optional>

namespace experimentalui {
// Purpose-drawn navigation symbols: consistent 18 px line icons in all
// palettes, without icon-font or KDE-theme dependencies. Active pages use
// the same accent as primary actions; icons have no semantic side effects.
inline QIcon navigationIcon(const QString &page) {
    const auto draw = [&page](const char *color) {
        QPixmap px(36, 36);
        px.setDevicePixelRatio(2.0);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(QString::fromLatin1(color)), 1.55,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (page == QLatin1String("Home")) {
            p.drawLine(QPointF(2.8, 8.8), QPointF(9, 3.2));
            p.drawLine(QPointF(9, 3.2), QPointF(15.2, 8.8));
            p.drawRoundedRect(QRectF(4.7, 8.3, 8.6, 7), 0.7, 0.7);
            p.drawLine(QPointF(9, 15.3), QPointF(9, 11.5));
        } else if (page == QLatin1String("Health")) {
            p.drawRoundedRect(QRectF(2, 3, 14, 12), 3, 3);
            p.drawPolyline(QPolygonF{QPointF(3.6, 9), QPointF(6, 9),
                QPointF(7.5, 6), QPointF(9.5, 12), QPointF(11, 8.5), QPointF(14.5, 8.5)});
        } else if (page == QLatin1String("Ryzen Curve") || page == QLatin1String("Intel Undervolt")) {
            p.drawRoundedRect(QRectF(4.5, 4.5, 9, 9), 1.6, 1.6);
            p.drawRect(QRectF(7.2, 7.2, 3.6, 3.6));
            for (int v : {6, 10, 13}) {
                p.drawLine(QPointF(v, 2), QPointF(v, 4.5));
                p.drawLine(QPointF(v, 13.5), QPointF(v, 16));
                p.drawLine(QPointF(2, v), QPointF(4.5, v));
                p.drawLine(QPointF(13.5, v), QPointF(16, v));
            }
        } else if (page == QLatin1String("NVIDIA Curve") || page == QLatin1String("AMD GPU")) {
            p.drawRoundedRect(QRectF(2, 4, 14, 10), 1.6, 1.6);
            p.drawEllipse(QPointF(7, 9), 2.5, 2.5);
            p.drawLine(QPointF(12, 6.5), QPointF(14.1, 6.5));
            p.drawLine(QPointF(12, 9), QPointF(14.1, 9));
            p.drawLine(QPointF(12, 11.5), QPointF(14.1, 11.5));
        } else if (page == QLatin1String("Optimizations")) {
            for (int y : {4, 9, 14}) p.drawLine(QPointF(2.5, y), QPointF(15.5, y));
            for (const QPointF &pos : {QPointF(6, 4), QPointF(12, 9), QPointF(8, 14)}) {
                p.setBrush(QColor(QString::fromLatin1(theme::BG1)));
                p.drawEllipse(pos, 1.8, 1.8);
            }
        } else if (page == QLatin1String("Scenes")) {
            p.drawRoundedRect(QRectF(2.8, 4, 10.5, 10.3), 1.5, 1.5);
            p.drawLine(QPointF(6, 2.5), QPointF(15.3, 2.5));
            p.drawLine(QPointF(15.3, 2.5), QPointF(15.3, 12));
            p.drawLine(QPointF(5.5, 8.5), QPointF(10.8, 8.5));
        } else if (page == QLatin1String("Lighting")) {
            p.drawEllipse(QRectF(5, 3.5, 8, 8));
            p.drawLine(QPointF(6.5, 13.2), QPointF(11.5, 13.2));
            p.drawLine(QPointF(7.2, 15.2), QPointF(10.8, 15.2));
            p.drawLine(QPointF(9, 1.2), QPointF(9, 2));
            p.drawLine(QPointF(3, 5), QPointF(4, 5.5));
            p.drawLine(QPointF(14, 5.5), QPointF(15, 5));
        } else {
            p.drawRoundedRect(QRectF(3.2, 2.7, 11.6, 12.6), 2, 2);
            for (int y : {6, 9, 12}) {
                p.drawLine(QPointF(6, y), QPointF(8, y));
                p.drawLine(QPointF(10.5, y), QPointF(12.3, y));
            }
        }
        p.end();
        return px;
    };
    QIcon icon;
    icon.addPixmap(draw(theme::FG_DIM), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(theme::ACCENT), QIcon::Normal, QIcon::On);
    icon.addPixmap(draw(theme::FG), QIcon::Active, QIcon::Off);
    icon.addPixmap(draw(theme::ACCENT), QIcon::Active, QIcon::On);
    return icon;
}

// One compact disclosure for an advanced/rarely-used section. Reparents neither
// widgets nor operations; the caller puts its existing panel immediately after it.
// Persisting disclosure state is intentionally left to the owning page.
inline void rememberSplitter(QSplitter *splitter, const QString &key) {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("centurion"), QStringLiteral("gui"));
    const QByteArray state = settings.value(key).toByteArray();
    if (!state.isEmpty()) splitter->restoreState(state);
    QObject::connect(splitter, &QSplitter::splitterMoved, splitter,
                     [splitter, key](int, int) {
        QSettings prefs(QSettings::IniFormat, QSettings::UserScope,
                        QStringLiteral("centurion"), QStringLiteral("gui"));
        prefs.setValue(key, splitter->saveState());
    });
}

inline QToolButton *disclosure(const QString &text, QWidget *panel, bool expanded = false) {
    auto *toggle = new QToolButton;
    toggle->setObjectName(QStringLiteral("expDisclosure"));
    toggle->setText(text);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // Full row: its top rule then spans the page like every other divider.
    toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    toggle->setCheckable(true);
    toggle->setChecked(expanded);
    panel->setVisible(expanded);
    QObject::connect(toggle, &QToolButton::toggled, panel, [panel, toggle](bool on) {
        panel->setVisible(on);
        toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    return toggle;
}

inline QLabel *eyebrow(const QString &s) {
    auto *l = new QLabel(s.toUpper());
    l->setObjectName(QStringLiteral("expEyebrow"));
    return l;
}
inline QLabel *subtitle(const QString &s) {
    auto *l = new QLabel(s);
    l->setProperty("role", "muted");
    l->setWordWrap(true);
    return l;
}
inline QFrame *strip() {
    auto *f = new QFrame;
    f->setObjectName(QStringLiteral("expStrip"));
    return f;
}
inline QFrame *surface() {
    auto *f = new QFrame;
    f->setObjectName(QStringLiteral("expSurface"));
    return f;
}
inline QLabel *metric(const QString &label, QLabel **value) {
    auto *box = new QLabel(label);
    box->setObjectName(QStringLiteral("expMetricLabel"));
    *value = new QLabel(QStringLiteral("—"));
    (*value)->setObjectName(QStringLiteral("expMetricValue"));
    return box;
}

// Graph drawn from existing samples; missing dGPU temperatures intentionally
// break the line rather than pretending an asleep GPU has a zero temperature.
class TemperaturePlot : public QWidget {
public:
    explicit TemperaturePlot(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(116);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setToolTip(QObject::tr("CPU and EC GPU temperatures, last 5 minutes. Missing samples are not estimated."));
    }
    void append(std::optional<double> cpu, std::optional<double> gpu) {
        samples_.append({cpu, gpu});
        while (samples_.size() > 150) samples_.removeFirst(); // 2s cadence, 5 min
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = QRectF(rect()).adjusted(39, 27, -12, -25);
        if (plot.width() < 30 || plot.height() < 30) return;
        const QColor muted(theme::MUTED), grid(theme::BORDER_SOFT);
        p.setPen(muted);
        p.drawText(QRectF(4, 3, 280, 19), Qt::AlignLeft, QObject::tr("TEMPERATURES  ·  LAST 5 MIN"));
        p.setPen(QPen(grid, 1));
        for (int t = 30; t <= 100; t += 10) {
            const double y = plot.bottom() - (t - 25.0) / 80.0 * plot.height();
            if (y < plot.top() || y > plot.bottom()) continue;
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(muted);
            p.drawText(QRectF(0, y - 9, 33, 18), Qt::AlignRight | Qt::AlignVCenter, QString::number(t));
            p.setPen(QPen(grid, 1));
        }
        for (int i = 0; i <= 5; ++i) {
            const qreal x = plot.left() + plot.width() * i / 5;
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        p.setPen(muted);
        p.drawText(QRectF(plot.left(), plot.bottom() + 3, 90, 17), Qt::AlignLeft, QObject::tr("-5 min"));
        p.drawText(QRectF(plot.right() - 90, plot.bottom() + 3, 90, 17), Qt::AlignRight, QObject::tr("Now"));
        if (samples_.size() < 2) {
            p.drawText(plot, Qt::AlignCenter, QObject::tr("Waiting for sensor samples"));
            return;
        }
        auto draw = [&](bool cpu, const QColor &color) {
            QPainterPath path;
            bool inSegment = false;
            for (int i = 0; i < samples_.size(); ++i) {
                const auto &value = cpu ? samples_[i].cpu : samples_[i].gpu;
                if (!value) { inSegment = false; continue; }
                const qreal x = plot.right() - (samples_.size() - 1 - i) * plot.width() / 149.0;
                const qreal y = plot.bottom() - (std::clamp(*value, 25.0, 105.0) - 25.0) / 80.0 * plot.height();
                if (inSegment) path.lineTo(x, y); else path.moveTo(x, y);
                inSegment = true;
            }
            p.setPen(QPen(color, 1.8));
            p.drawPath(path);
        };
        draw(true, QColor(theme::ACCENT));
        draw(false, QColor(theme::OK));
        p.setPen(QColor(theme::ACCENT));
        p.drawText(QRectF(plot.right() - 125, 3, 48, 19), Qt::AlignLeft, QStringLiteral("● CPU"));
        p.setPen(QColor(theme::OK));
        p.drawText(QRectF(plot.right() - 66, 3, 60, 19), Qt::AlignLeft, QStringLiteral("● GPU"));
    }
private:
    struct Sample { std::optional<double> cpu, gpu; };
    QVector<Sample> samples_;
};
}
