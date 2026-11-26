#pragma once
// Experimental UI primitives. Presentation only: no hardware access and no polling.
// Classic mode never instantiates these widgets.
#include "theme.h"
#include <QFrame>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QIcon>
#include <QLinearGradient>
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
        QPixmap px(64, 64);
        px.setDevicePixelRatio(2.0);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.scale(32.0 / 18, 32.0 / 18);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(QString::fromLatin1(color)), 1.55,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (page == QLatin1String("Ryzen Curve") || page == QLatin1String("AMD GPU")) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#ED1C24"));
            p.drawPolygon(QPolygonF{{2,2},{16,2},{16,16},{12,12},{12,6},{6,6}});
            p.drawPolygon(QPolygonF{{2,7},{6,11},{6,12},{10,12},{14,16},{2,16}});
        } else if (page == QLatin1String("NVIDIA Curve")) {
            p.setPen(QPen(QColor("#76B900"), 1.7));
            QPainterPath eye;
            eye.moveTo(1,9);
            eye.cubicTo(5,2,12,2,17,9);
            eye.cubicTo(13,15,6,16,3,10);
            eye.cubicTo(6,6,11,5,14,9);
            eye.cubicTo(11,12,8,12,7,9);
            p.drawPath(eye);
        } else if (page == QLatin1String("Memory")) {
            p.drawRoundedRect(QRectF(1.5,5,15,8),1,1);
            for (int x : {4,8,12}) p.drawRect(QRectF(x,7,2,3));
            for (int x : {4,6,8,12,14}) p.drawLine(QPointF(x,13),QPointF(x,15));
        } else if (page == QLatin1String("GPU")) {
            p.drawRoundedRect(QRectF(2,4,14,10),1,1);
            p.drawEllipse(QPointF(7,9),3,3);
            p.drawLine(QPointF(12,7),QPointF(14,7));
        } else if (page == QLatin1String("Favorites")) {
            p.drawPolygon(QPolygonF{{9,2},{11,6.5},{16,7},{12.3,10.5},{13.3,15.6},
                                    {9,13.2},{4.7,15.6},{5.7,10.5},{2,7},{7,6.5}});
        } else if (page == QLatin1String("Operations")) {
            for (int y : {4,9,14}) {
                p.drawPoint(QPointF(3,y));
                p.drawLine(QPointF(6,y), QPointF(15,y));
            }
        } else if (page == QLatin1String("Collapse") || page == QLatin1String("Expand")) {
            p.drawRoundedRect(QRectF(2,3,14,12), 1.5, 1.5);
            p.drawLine(QPointF(6,3), QPointF(6,15));
            const int x = page == QLatin1String("Collapse") ? 12 : 9;
            p.drawPolyline(QPolygonF{{qreal(x),6},{qreal(21-x),9},{qreal(x),12}});
        } else if (page == QLatin1String("Home")) {
            p.drawLine(QPointF(2.8, 8.8), QPointF(9, 3.2));
            p.drawLine(QPointF(9, 3.2), QPointF(15.2, 8.8));
            p.drawRoundedRect(QRectF(4.7, 8.3, 8.6, 7), 0.7, 0.7);
            p.drawLine(QPointF(9, 15.3), QPointF(9, 11.5));
        } else if (page == QLatin1String("Health")) {
            p.drawRoundedRect(QRectF(2, 3, 14, 12), 3, 3);
            p.drawPolyline(QPolygonF{QPointF(3.6, 9), QPointF(6, 9),
                QPointF(7.5, 6), QPointF(9.5, 12), QPointF(11, 8.5), QPointF(14.5, 8.5)});
        } else if (page == QLatin1String("Intel Undervolt")) {
            p.drawRoundedRect(QRectF(4.5, 4.5, 9, 9), 1.6, 1.6);
            p.drawRect(QRectF(7.2, 7.2, 3.6, 3.6));
            for (int v : {6, 10, 13}) {
                p.drawLine(QPointF(v, 2), QPointF(v, 4.5));
                p.drawLine(QPointF(v, 13.5), QPointF(v, 16));
                p.drawLine(QPointF(2, v), QPointF(4.5, v));
                p.drawLine(QPointF(13.5, v), QPointF(16, v));
            }
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
    const char *color = theme::ACCENT;
    if (page == "Intel Undervolt") color = "#007DC3";
    else if (page == "Health" || page == "GPU") color = theme::OK;
    else if (page == "Memory" || page == "Scenes") color = theme::PURPLE;
    else if (page == "Firmware" || page == "Optimizations") color = theme::INFO;
    else if (page == "Lighting") color = theme::WARN;
    QIcon icon(draw(color));
    return icon;
}

// Magnifier for the top bar's search field and the navigator, same stroke as the page icons.
inline QIcon searchIcon(const char *color) {
    QPixmap px(32, 32);
    px.setDevicePixelRatio(2.0);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(QString::fromLatin1(color)), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(QPointF(7, 7), 4.3, 4.3);
    p.drawLine(QPointF(10.3, 10.3), QPointF(13.8, 13.8));
    p.end();
    return QIcon(px);
}

// Status dot (top bar's power-profile chip): the colour with a soft halo.
inline QIcon dotIcon(const QColor &color) {
    QPixmap px(20, 20);
    px.setDevicePixelRatio(2.0);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QColor halo = color;
    halo.setAlphaF(0.25);
    p.setBrush(halo);
    p.drawEllipse(QPointF(5, 5), 5, 5);
    p.setBrush(color);
    p.drawEllipse(QPointF(5, 5), 3, 3);
    p.end();
    return QIcon(px);
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

// Graph drawn from existing samples; missing dGPU temperatures intentionally
// break the line rather than pretending an asleep GPU has a zero temperature.
class TemperaturePlot : public QWidget {
public:
    explicit TemperaturePlot(QWidget *parent = nullptr) : QWidget(parent) {
        clock_.start();
        setMinimumHeight(std::max(116, fontMetrics().height() * 6));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(QObject::tr("Temperature history"));
        setToolTip(QObject::tr("CPU and EC GPU temperatures. Missing samples and gaps are not estimated."));
    }
    void append(std::optional<double> cpu, std::optional<double> gpu) {
        const qint64 now = clock_.elapsed();
        samples_.append({now, cpu, gpu});
        while (!samples_.isEmpty() && (samples_.size() > 2048 || samples_.first().ms < now - 3600000)) samples_.removeFirst();
        update();
    }
    void setRangeMinutes(int minutes) {
        rangeMinutes_ = std::clamp(minutes, 1, 60);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const int left = fontMetrics().horizontalAdvance(QStringLiteral("100")) + 12;
        const int lineHeight = fontMetrics().height() + 6;
        const QRectF plot = QRectF(rect()).adjusted(left, lineHeight + 5, -12, -lineHeight);
        const qint64 now = clock_.elapsed(), rangeMs = qint64(rangeMinutes_) * 60000;
        const qint64 firstMs = now - rangeMs;
        if (plot.width() < 30 || plot.height() < 30) return;
        const QColor muted(theme::MUTED), grid(theme::BORDER_SOFT);
        QFont caption = font();
        if (caption.pointSizeF() > 2) caption.setPointSizeF(caption.pointSizeF() - 1);
        caption.setWeight(QFont::DemiBold);
        caption.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
        p.setFont(caption);
        p.setPen(muted);
        if (width() > 400) p.drawText(QRectF(4, 3, width() / 2, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("Temperatures"));
        caption.setLetterSpacing(QFont::AbsoluteSpacing, 0);
        caption.setWeight(QFont::Normal);
        p.setFont(caption);
        // Horizontal rules only (dashed): the eye follows the level, time needs no grid.
        // Short plot (Home's top row): a label every 20 °C, so they never touch.
        const int step = plot.height() < 140 ? 20 : 10;
        for (int t = step == 20 ? 40 : 30; t <= 100; t += step) {
            const double y = plot.bottom() - (t - 25.0) / 80.0 * plot.height();
            if (y < plot.top() || y > plot.bottom()) continue;
            p.setPen(QPen(grid, 1, Qt::DashLine));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(muted);
            p.drawText(QRectF(0, y - lineHeight / 2, left - 6, lineHeight), Qt::AlignRight | Qt::AlignVCenter, QString::number(t));
        }
        p.setPen(QPen(QColor(theme::BORDER), 1));
        p.drawLine(plot.bottomLeft(), plot.bottomRight());
        p.setPen(muted);
        p.drawText(QRectF(plot.left(), plot.bottom() + 3, 90, lineHeight), Qt::AlignLeft, QObject::tr("-%1 min").arg(rangeMinutes_));
        p.drawText(QRectF(plot.right() - 90, plot.bottom() + 3, 90, lineHeight), Qt::AlignRight, QObject::tr("Now"));
        if (samples_.size() < 2) {
            p.drawText(plot, Qt::AlignCenter, QObject::tr("Waiting for sensor samples"));
            return;
        }
        // Legend, right-aligned on the caption line, with the live reading of each series.
        qreal legendRight = plot.right();
        auto draw = [&](bool cpu, const QColor &color, const QString &name) {
            QPainterPath path;
            bool inSegment = false;
            qreal segmentStart = 0;
            std::optional<double> last;
            // A soft wash under each unbroken run of samples, fading to the baseline.
            QLinearGradient wash(plot.topLeft(), plot.bottomLeft());
            QColor top = color, bottom = color;
            top.setAlphaF(0.20);
            bottom.setAlphaF(0.0);
            wash.setColorAt(0, top);
            wash.setColorAt(1, bottom);
            QPainterPath run;
            auto closeRun = [&](qreal endX) {
                if (run.elementCount() < 2) { run = QPainterPath(); return; }
                run.lineTo(endX, plot.bottom());
                run.lineTo(segmentStart, plot.bottom());
                run.closeSubpath();
                p.fillPath(run, wash);
                run = QPainterPath();
            };
            qreal x = 0;
            qint64 previousMs = -1;
            for (const auto &sample : samples_) {
                if (sample.ms < firstMs) continue;
                const auto &value = cpu ? sample.cpu : sample.gpu;
                if (!value || (previousMs >= 0 && sample.ms - previousMs > 6000)) {
                    if (inSegment) closeRun(x);
                    inSegment = false;
                }
                previousMs = sample.ms;
                if (!value) continue;
                x = plot.right() - (now - sample.ms) * plot.width() / double(rangeMs);
                const qreal y = plot.bottom() - (std::clamp(*value, 25.0, 105.0) - 25.0) / 80.0 * plot.height();
                if (inSegment) { path.lineTo(x, y); run.lineTo(x, y); }
                else { path.moveTo(x, y); run.moveTo(x, y); segmentStart = x; }
                inSegment = true;
                last = value;
            }
            if (inSegment) closeRun(x);
            p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
            const bool live = inSegment && !samples_.isEmpty() && now - samples_.last().ms <= 6000;
            if (live) {  // the live reading: a dot with a halo
                QColor halo = color;
                halo.setAlphaF(0.25);
                p.setPen(Qt::NoPen);
                p.setBrush(halo);
                p.drawEllipse(path.currentPosition(), 5.5, 5.5);
                p.setBrush(color);
                p.drawEllipse(path.currentPosition(), 2.6, 2.6);
                p.setBrush(Qt::NoBrush);
            }
            // "● CPU 62°": the value only while the series is live (an asleep dGPU has none).
            const QString text = live && last ? QStringLiteral("%1  %2°").arg(name).arg(qRound(*last)) : name;
            QFont f = p.font();
            f.setWeight(QFont::DemiBold);
            p.setFont(f);
            const qreal w = p.fontMetrics().horizontalAdvance(text);
            const QRectF label(legendRight - w, 3, w, lineHeight);
            p.setPen(live ? QColor(theme::FG) : muted);
            p.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, text);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(label.left() - 9, label.center().y()), 3.2, 3.2);
            p.setBrush(Qt::NoBrush);
            legendRight = label.left() - 28;
        };
        draw(false, QColor(theme::OK), QStringLiteral("GPU"));
        draw(true, QColor(theme::ACCENT), QStringLiteral("CPU"));
    }
private:
    struct Sample { qint64 ms; std::optional<double> cpu, gpu; };
    QVector<Sample> samples_;
    QElapsedTimer clock_;
    int rangeMinutes_ = 5;
};
}
