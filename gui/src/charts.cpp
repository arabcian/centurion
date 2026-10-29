#include "charts.h"
#include "theme.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

constexpr int BAR_ROW = 20;

QColor colorFrom(const char *hex) { return QColor(hex); }

} // namespace

// ── LineChart ────────────────────────────────────────────────────────────────

LineChart::LineChart(QWidget *parent) : QWidget(parent) {}

void LineChart::setPoints(const QVector<Point> &points, const QString &yUnit) {
    points_ = points;
    unit_ = yUnit;
    update();
}

void LineChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (points_.size() < 2) {
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(rect(), Qt::AlignCenter, tr("The line appears with the second sample."));
        return;
    }
    const QRectF r = QRectF(rect()).adjusted(44, 8, -10, -22);
    double x0 = points_.first().x, x1 = points_.first().x, y0 = points_.first().y, y1 = points_.first().y;
    for (const Point &pt : points_) {
        x0 = std::min(x0, pt.x); x1 = std::max(x1, pt.x);
        y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y);
    }
    // Whole percent (or unit) rows around the data, at least one unit tall.
    const double lo = std::floor(y0), hi = std::max(std::ceil(y1), lo + 1);
    auto X = [&](double x) { return r.left() + (x1 > x0 ? (x - x0) / (x1 - x0) : 0.5) * r.width(); };
    auto Y = [&](double y) { return r.bottom() - (y - lo) / (hi - lo) * r.height(); };

    p.setPen(QPen(colorFrom(theme::BORDER_SOFT)));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.drawLine(r.topLeft(), r.bottomLeft());
    p.setPen(colorFrom(theme::MUTED));
    p.drawText(QRectF(0, Y(hi) - 8, 40, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1%2").arg(hi, 0, 'f', 0).arg(unit_));
    p.drawText(QRectF(0, Y(lo) - 8, 40, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1%2").arg(lo, 0, 'f', 0).arg(unit_));
    p.drawText(QRectF(r.left(), r.bottom() + 3, 120, 16), Qt::AlignLeft, points_.first().label);
    p.drawText(QRectF(r.right() - 120, r.bottom() + 3, 120, 16), Qt::AlignRight, points_.last().label);

    QPainterPath path;
    for (int i = 0; i < points_.size(); ++i) {
        const QPointF q(X(points_[i].x), Y(points_[i].y));
        if (i == 0) path.moveTo(q); else path.lineTo(q);
    }
    p.setPen(QPen(colorFrom(theme::ACCENT), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(colorFrom(theme::ACCENT));
    for (const Point &pt : points_) p.drawEllipse(QPointF(X(pt.x), Y(pt.y)), 2.5, 2.5);
}

// ── BarChart ─────────────────────────────────────────────────────────────────

BarChart::BarChart(QWidget *parent) : QWidget(parent) {}

void BarChart::setBars(const QVector<Bar> &bars) {
    bars_ = bars;
    setMinimumHeight(bars_.size() * BAR_ROW + 4);
    update();
}

void BarChart::paintEvent(QPaintEvent *) {
    if (bars_.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(font());
    int labelW = 0, textW = 0;
    double maxV = 0;
    for (const Bar &b : bars_) {
        labelW = std::max(labelW, fm.horizontalAdvance(b.label));
        textW = std::max(textW, fm.horizontalAdvance(b.text));
        maxV = std::max(maxV, b.value);
    }
    labelW = std::min(labelW, width() / 2) + 10;
    const int barMax = std::max(20, width() - labelW - textW - 16);
    for (int i = 0; i < bars_.size(); ++i) {
        const Bar &b = bars_[i];
        const int y = i * BAR_ROW;
        p.setPen(colorFrom(theme::FG_DIM));
        p.drawText(QRect(0, y, labelW - 6, BAR_ROW), Qt::AlignRight | Qt::AlignVCenter, fm.elidedText(b.label, Qt::ElideRight, labelW - 6));
        const int len = maxV > 0 ? int(std::round(barMax * b.value / maxV)) : 0;
        p.setPen(Qt::NoPen);
        p.setBrush(colorFrom(theme::ACCENT_SOFT));
        p.drawRoundedRect(QRect(labelW, y + 4, std::max(len, 2), BAR_ROW - 8), 2, 2);
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(QRect(labelW + len + 6, y, textW + 4, BAR_ROW), Qt::AlignLeft | Qt::AlignVCenter, b.text);
    }
}

// ── DayBars ──────────────────────────────────────────────────────────────────

DayBars::DayBars(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void DayBars::setDays(const QVector<Day> &days) {
    days_ = days;
    update();
}

void DayBars::setSelected(const QString &date) {
    selected_ = date;
    update();
}

QColor DayBars::colorOf(const QString &kind) {
    static const char *const PALETTE[] = {theme::INFO, theme::WARN, theme::DANGER, theme::OK, theme::PURPLE,
                                          theme::ACCENT, theme::FG_DIM, theme::ACCENT_SOFT, theme::DANGER_SOFT};
    int sum = 0;  // not qHash: its seed changes between runs
    for (const QChar c : kind) sum += c.unicode();
    return colorFrom(PALETTE[sum % int(std::size(PALETTE))]);
}

QRectF DayBars::plotRect() const { return QRectF(rect()).adjusted(4, 4, -4, -18); }

int DayBars::columnAt(const QPointF &p) const {
    const QRectF r = plotRect();
    if (days_.isEmpty() || !r.contains(p)) return -1;
    return std::min<int>(days_.size() - 1, int((p.x() - r.left()) / (r.width() / days_.size())));
}

void DayBars::paintEvent(QPaintEvent *) {
    if (days_.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    int maxTotal = 1;
    for (const Day &d : days_) {
        int total = 0;
        for (int n : d.kinds) total += n;
        maxTotal = std::max(maxTotal, total);
    }
    const double colW = r.width() / days_.size();
    const double barW = std::max(2.0, colW * 0.7);
    for (int i = 0; i < days_.size(); ++i) {
        const Day &d = days_[i];
        const double x = r.left() + i * colW + (colW - barW) / 2;
        double yBottom = r.bottom();
        for (auto it = d.kinds.cbegin(); it != d.kinds.cend(); ++it) {
            if (it.value() <= 0) continue;
            const double h = r.height() * it.value() / maxTotal;
            p.setPen(Qt::NoPen);
            p.setBrush(colorOf(it.key()));
            p.drawRect(QRectF(x, yBottom - h, barW, h));
            yBottom -= h;
        }
        if (d.date == selected_) {
            p.setPen(QPen(colorFrom(theme::FG), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(r.left() + i * colW, r.top(), colW, r.height()));
        }
    }
    p.setPen(QPen(colorFrom(theme::BORDER_SOFT)));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.setPen(colorFrom(theme::MUTED));
    p.drawText(QRectF(r.left(), r.bottom() + 2, 80, 14), Qt::AlignLeft, days_.first().date.mid(5));
    p.drawText(QRectF(r.right() - 80, r.bottom() + 2, 80, 14), Qt::AlignRight, days_.last().date.mid(5));
}

void DayBars::mousePressEvent(QMouseEvent *e) {
    const int i = columnAt(e->position());
    if (i >= 0) Q_EMIT dayClicked(days_[i].date);
}

void DayBars::mouseMoveEvent(QMouseEvent *e) {
    const int i = columnAt(e->position());
    if (i < 0) { setToolTip(QString()); return; }
    QStringList parts;
    for (auto it = days_[i].kinds.cbegin(); it != days_[i].kinds.cend(); ++it) parts << QStringLiteral("%1 ×%2").arg(it.key()).arg(it.value());
    setToolTip(QStringLiteral("%1  %2").arg(days_[i].date, parts.isEmpty() ? tr("no events") : parts.join(QStringLiteral(", "))));
}
