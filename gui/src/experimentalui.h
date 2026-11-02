#pragma once
// Experimental UI primitives. Presentation only: no hardware access and no polling.
// Classic mode never instantiates these widgets.
#include "theme.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <optional>

namespace experimentalui {
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
        setMinimumHeight(156);
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
