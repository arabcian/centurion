#include "charts.h"
#include "theme.h"

#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QSaveFile>
#include <QTextStream>
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

int barRowHeight(const QFontMetrics &fm) { return std::max(20, fm.height() + 8); }

QColor colorFrom(const char *hex) { return QColor(hex); }

} // namespace

// ── LineChart ────────────────────────────────────────────────────────────────

LineChart::LineChart(QWidget *parent) : QWidget(parent) { setAccessibleName(QCoreApplication::translate("LineChart", "Line chart")); setMouseTracking(true); }

void LineChart::setPoints(const QVector<Point> &points, const QString &yUnit) {
    points_.clear();
    for (const auto &pt : points) if (std::isfinite(pt.x) && std::isfinite(pt.y)) points_.append(pt);
    plotted_.clear(); hover_ = -1;
    unit_ = yUnit;
    // Screen readers cannot see the painting: the description carries the numbers.
    setAccessibleDescription(points_.isEmpty() ? QString()
        : QCoreApplication::translate("LineChart", "%1 points, %2 to %3; last value %4 %5")
              .arg(points_.size()).arg(points_.first().label, points_.last().label).arg(points_.last().y).arg(yUnit));
    update();
}

void LineChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    plotted_.clear();
    if (points_.size() < 2) {
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(rect(), Qt::AlignCenter, tr("The line appears with the second sample."));
        return;
    }
    double x0 = points_.first().x, x1 = points_.first().x, y0 = points_.first().y, y1 = points_.first().y;
    for (const Point &pt : points_) {
        x0 = std::min(x0, pt.x); x1 = std::max(x1, pt.x);
        y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y);
    }
    // Whole percent (or unit) rows around the data, at least one unit tall.
    const double lo = std::floor(y0), hi = std::max(std::ceil(y1), lo + 1);
    const QFontMetrics fm(font());
    const QString high = QStringLiteral("%1%2").arg(hi, 0, 'f', 0).arg(unit_);
    const QString low = QStringLiteral("%1%2").arg(lo, 0, 'f', 0).arg(unit_);
    const int axis = std::max(fm.horizontalAdvance(high), fm.horizontalAdvance(low)) + 8;
    const int rowH = fm.height();
    const QRectF r = QRectF(rect()).adjusted(axis, rowH / 2 + 2, -10, -rowH - 5);
    if (r.width() <= 0 || r.height() <= 0) return;
    auto X = [&](double x) { return r.left() + (x1 > x0 ? (x - x0) / (x1 - x0) : 0.5) * r.width(); };
    auto Y = [&](double y) { return r.bottom() - (y - lo) / (hi - lo) * r.height(); };

    p.setPen(QPen(colorFrom(theme::BORDER_SOFT)));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.drawLine(r.topLeft(), r.bottomLeft());
    p.setPen(colorFrom(theme::MUTED));
    p.drawText(QRectF(0, Y(hi) - rowH / 2.0, axis - 4, rowH), Qt::AlignRight | Qt::AlignVCenter, high);
    p.drawText(QRectF(0, Y(lo) - rowH / 2.0, axis - 4, rowH), Qt::AlignRight | Qt::AlignVCenter, low);
    const int half = int(r.width() / 2);
    p.drawText(QRectF(r.left(), r.bottom() + 3, half, rowH), Qt::AlignLeft, fm.elidedText(points_.first().label, Qt::ElideRight, half));
    p.drawText(QRectF(r.right() - half, r.bottom() + 3, half, rowH), Qt::AlignRight, fm.elidedText(points_.last().label, Qt::ElideLeft, half));

    QPainterPath path;
    for (int i = 0; i < points_.size(); ++i) {
        const QPointF q(X(points_[i].x), Y(points_[i].y));
        plotted_.append(q);
        if (i == 0) path.moveTo(q); else path.lineTo(q);
    }
    p.setPen(QPen(colorFrom(theme::ACCENT), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(colorFrom(theme::ACCENT));
    for (const QPointF &pt : plotted_) p.drawEllipse(pt, 2.5, 2.5);
    if (hover_ >= 0 && hover_ < plotted_.size()) {
        const auto pt = plotted_.at(hover_);
        p.setPen(QPen(colorFrom(theme::FG_DIM), 1, Qt::DashLine));
        p.drawLine(QPointF(pt.x(), r.top()), QPointF(pt.x(), r.bottom()));
        p.setPen(QPen(colorFrom(theme::FG), 1)); p.setBrush(colorFrom(theme::ACCENT)); p.drawEllipse(pt, 4, 4);
    }
}

void LineChart::mouseMoveEvent(QMouseEvent *e) {
    const auto pos = e->position();
    int best = -1; double distance = 144;
    for (int i=0; i<plotted_.size(); ++i) {
        const auto d = plotted_.at(i) - pos; const double d2=d.x()*d.x()+d.y()*d.y();
        if (d2 < distance) { best=i; distance=d2; }
    }
    if (best != hover_) { hover_=best; update(); }
    if (best >= 0) {
        const auto &pt=points_.at(best);
        QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("%1\n%2%3").arg(pt.label.toHtmlEscaped()).arg(pt.y,0,'g',10).arg(unit_.toHtmlEscaped()), this);
    } else QToolTip::hideText();
}
void LineChart::leaveEvent(QEvent *e) {
    hover_=-1; QToolTip::hideText(); update(); QWidget::leaveEvent(e);
}
void LineChart::contextMenuEvent(QContextMenuEvent *e) {
    QMenu menu(this);
    auto *exportAction=menu.addAction(QCoreApplication::translate("LineChart", "Export plotted samples…"));
    exportAction->setEnabled(!points_.isEmpty());
    if (menu.exec(e->globalPos()) != exportAction) return;
    const QString path=QFileDialog::getSaveFileName(this,QCoreApplication::translate("LineChart", "Export plotted samples"),{},QStringLiteral("CSV (*.csv)"));
    if(path.isEmpty())return;
    // Export numeric samples only; labels can contain untrusted spreadsheet formula prefixes.
    QSaveFile f(path);
    if(!f.open(QIODevice::WriteOnly)) {QMessageBox::warning(this,QCoreApplication::translate("LineChart", "Export failed"),f.errorString());return;}
    QTextStream out(&f); out.setRealNumberPrecision(17); out << "x,y\n";
    for(const auto &pt:points_)out<<pt.x<<','<<pt.y<<'\n';
    out.flush();
    if(out.status()!=QTextStream::Ok||!f.commit())QMessageBox::warning(this,QCoreApplication::translate("LineChart", "Export failed"),f.errorString());
}

// ── BarChart ─────────────────────────────────────────────────────────────────

BarChart::BarChart(QWidget *parent) : QWidget(parent) { setAccessibleName(QCoreApplication::translate("BarChart", "Bar chart")); }

void BarChart::setBars(const QVector<Bar> &bars) {
    bars_ = bars;
    QStringList rows;
    for (const Bar &b : bars) rows << b.label + QStringLiteral(": ") + b.text;
    setAccessibleDescription(rows.join(QStringLiteral("; ")));
    setMinimumHeight(int(bars_.size()) * barRowHeight(fontMetrics()) + 4);
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
    const int rowH = barRowHeight(fm);
    if (width() < 30) return;
    labelW = std::min(labelW + 10, width() / 2);
    textW = std::min(textW + 4, width() / 3);
    const int barMax = std::max(0, width() - labelW - textW - 8);
    for (int i = 0; i < bars_.size(); ++i) {
        const Bar &b = bars_[i];
        const int y = i * rowH;
        p.setPen(colorFrom(theme::FG_DIM));
        p.drawText(QRect(0, y, labelW - 6, rowH), Qt::AlignRight | Qt::AlignVCenter, fm.elidedText(b.label, Qt::ElideRight, labelW - 6));
        const int len = maxV > 0 && std::isfinite(b.value) ? int(std::round(barMax * std::clamp(b.value / maxV, 0.0, 1.0))) : 0;
        p.setPen(Qt::NoPen);
        p.setBrush(colorFrom(theme::ACCENT_SOFT));
        if (len > 0) p.drawRoundedRect(QRect(labelW, y + 4, len, rowH - 8), 2, 2);
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(QRect(width() - textW, y, textW, rowH), Qt::AlignRight | Qt::AlignVCenter, fm.elidedText(b.text, Qt::ElideRight, textW));
    }
}

// ── DayBars ──────────────────────────────────────────────────────────────────

DayBars::DayBars(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Kernel events per day"));
}

void DayBars::setDays(const QVector<Day> &days) {
    days_ = days;
    QStringList rows;
    for (const Day &d : days) {
        int n = 0;
        for (int c : d.kinds) n += c;
        if (n) rows << tr("%1: %n event(s)", nullptr, n).arg(d.date);
    }
    setAccessibleDescription(rows.join(QStringLiteral("; ")));
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
    if (days_.isEmpty() || r.width() <= 0 || r.height() <= 0 || !r.contains(p)) return -1;
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
