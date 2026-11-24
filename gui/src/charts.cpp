#include "charts.h"
#include "chartaxis.h"
#include <QAbstractTableModel>
#include <QVariant>
#include <utility>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
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
#include <QWheelEvent>
#include <QKeyEvent>
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

// A snapshot model, not one QWidget/item per sample; opening a large table is bounded by the data copy.
class SampleModel final : public QAbstractTableModel {
public:
    SampleModel(QVector<LineChart::Point> points, QString unit, QObject *parent)
        : QAbstractTableModel(parent), points_(std::move(points)), unit_(std::move(unit)) {}
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : int(points_.size()); }
    int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : 4; }
    QVariant data(const QModelIndex &i, int role) const override {
        if (!i.isValid() || i.row()<0 || i.row()>=points_.size() || role!=Qt::DisplayRole) return {};
        const auto &p=points_.at(i.row());
        switch(i.column()) {
        case 0: return p.label;
        case 1: return QString::number(p.x,'g',17);
        case 2: return QString::number(p.y,'g',17);
        case 3: return p.breakBefore ? QCoreApplication::translate("LineChart", "Gap before sample") : QString();
        default: return {};
        }
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation!=Qt::Horizontal || role!=Qt::DisplayRole) return QAbstractTableModel::headerData(section,orientation,role);
        switch(section) {
        case 0: return QCoreApplication::translate("LineChart", "Sample");
        case 1: return QStringLiteral("X");
        case 2: return QStringLiteral("Y (%1)").arg(unit_);
        case 3: return QCoreApplication::translate("LineChart", "Continuity");
        default: return {};
        }
    }
private:
    QVector<LineChart::Point> points_; QString unit_;
};

} // namespace

// ── LineChart ────────────────────────────────────────────────────────────────

LineChart::LineChart(QWidget *parent) : QWidget(parent) { setAccessibleName(QCoreApplication::translate("LineChart", "Line chart")); setMouseTracking(true); setFocusPolicy(Qt::StrongFocus); }

void LineChart::setPoints(const QVector<Point> &points, const QString &yUnit) {
    points_.clear();
    bool gap = false;
    for (const auto &pt : points) {
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) { gap = true; continue; }
        auto copy = pt; copy.breakBefore = copy.breakBefore || gap;
        points_.append(copy); gap = false;
    }
    begin_ = 0; end_ = points_.size();
    plotted_.clear(); plottedIndices_.clear(); hover_ = -1;
    unit_ = yUnit;
    // Screen readers cannot see the painting: the description carries the numbers.
    setAccessibleDescription(points_.isEmpty() ? QString()
        : QCoreApplication::translate("LineChart", "%1 points, %2 to %3; last value %4 %5")
              .arg(points_.size()).arg(points_.first().label, points_.last().label).arg(points_.last().y).arg(yUnit));
    update();
}

void LineChart::setViewRange(qsizetype begin, qsizetype end) {
    begin_ = std::clamp(begin, qsizetype(0), points_.size());
    end_ = std::clamp(end, begin_, points_.size());
    if (begin_ == end_ && !points_.isEmpty()) { begin_ = std::min(begin_,points_.size()-1); end_ = begin_+1; }
    plotted_.clear(); plottedIndices_.clear(); hover_ = -1; update();
}

void LineChart::wheelEvent(QWheelEvent *e) {
    if (!(e->modifiers() & Qt::ControlModifier) || end_-begin_<1 || e->angleDelta().y()==0) { e->ignore(); return; }
    const qsizetype count=end_-begin_;
    const qsizetype target=e->angleDelta().y()>0 ? std::max(qsizetype(2),count*3/4) : std::min(points_.size(),count+std::max(qsizetype(1),count/3));
    const auto length=std::min(points_.size(),target);
    const auto first=std::clamp(begin_+(count-length)/2,qsizetype(0),points_.size()-length);
    setViewRange(first,first+length); e->accept();
}
void LineChart::keyPressEvent(QKeyEvent *e) {
    if (e->key()==Qt::Key_Home) { resetView(); e->accept(); return; }
    if (e->key()==Qt::Key_Left || e->key()==Qt::Key_Right) {
        const auto count=end_-begin_;
        const auto shift=std::max(qsizetype(1),count/5)*(e->key()==Qt::Key_Left?-1:1);
        const auto first=std::clamp(begin_+shift,qsizetype(0),points_.size()-count);
        setViewRange(first,first+count); e->accept(); return;
    }
    QWidget::keyPressEvent(e);
}

void LineChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    plotted_.clear(); plottedIndices_.clear();
    if (begin_ >= end_) {
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(rect(), Qt::AlignCenter, QCoreApplication::translate("LineChart", "No recorded samples."));
        return;
    }
    double x0 = points_[begin_].x, x1 = x0, y0 = points_[begin_].y, y1 = y0;
    for (qsizetype i=begin_; i<end_; ++i) {
        const auto &pt=points_[i];
        x0 = std::min(x0, pt.x); x1 = std::max(x1, pt.x);
        y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y);
    }
    const auto xr = chartaxis::Range::make(x0,x1), yr = chartaxis::Range::make(y0,y1);
    if (!xr || !yr) return;
    const auto ticks = yr->ticks(unsigned(std::clamp(height() / 60, 1, 4)));
    const QFontMetrics fm(font());
    QStringList labels;
    int axis = 0;
    for (const double tick : ticks) {
        const auto label = QString::number(tick, 'g', 17) + unit_;
        // Use shorter labels unless distinct ticks would become indistinguishable.
        labels << label;
    }
    QStringList shortLabels;
    for (const double tick : ticks) shortLabels << QString::number(tick, 'g', 6) + unit_;
    auto uniqueLabels = shortLabels; uniqueLabels.removeDuplicates();
    if (uniqueLabels.size() == shortLabels.size()) labels = shortLabels;
    for (const auto &label : labels) axis = std::max(axis, fm.horizontalAdvance(label));
    axis = std::min(axis + 8, width() / 2);
    const int rowH = fm.height();
    const QRectF r = QRectF(rect()).adjusted(axis, rowH / 2 + 2, -10, -rowH - 5);
    if (r.width() <= 0 || r.height() <= 0) return;
    auto X = [&](double x) { return r.left() + xr->fraction(x) * r.width(); };
    auto Y = [&](double y) { return r.bottom() - yr->fraction(y)*r.height(); };
    p.setPen(QPen(colorFrom(theme::BORDER_SOFT)));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.drawLine(r.topLeft(), r.bottomLeft());
    for (size_t i=0; i<ticks.size(); ++i) {
        const double y=Y(ticks[i]);
        p.setPen(QPen(colorFrom(theme::BORDER_SOFT),1,Qt::DotLine));
        p.drawLine(QPointF(r.left(),y),QPointF(r.right(),y));
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(QRectF(0,y-rowH/2.0,axis-4,rowH),Qt::AlignRight|Qt::AlignVCenter,
                   fm.elidedText(labels.at(qsizetype(i)),Qt::ElideRight,axis-4));
    }
    const int half = int(r.width() / 2);
    p.drawText(QRectF(r.left(), r.bottom() + 3, half, rowH), Qt::AlignLeft, fm.elidedText(points_[begin_].label, Qt::ElideRight, half));
    p.drawText(QRectF(r.right() - half, r.bottom() + 3, half, rowH), Qt::AlignRight, fm.elidedText(points_[end_-1].label, Qt::ElideLeft, half));

    QPainterPath path;
    // Each bucket retains its first/last and extrema. If any gap occurs inside it,
    // break the line instead of inventing continuity through unavailable readings.
    const qsizetype bucket=std::max(qsizetype(1),(end_-begin_+std::max(1,width())-1)/std::max(1,width()));
    for (qsizetype i=begin_; i<end_; i+=bucket) {
        const qsizetype last=std::min(end_,i+bucket)-1;
        qsizetype minAt=i,maxAt=i; bool gap=false;
        for(qsizetype j=i;j<=last;++j) {
            if(points_[j].y<points_[minAt].y) minAt=j;
            if(points_[j].y>points_[maxAt].y) maxAt=j;
            gap|=points_[j].breakBefore;
        }
        QList<qsizetype> indices{i,minAt,maxAt,last}; std::sort(indices.begin(),indices.end());
        indices.erase(std::unique(indices.begin(),indices.end()),indices.end());
        for(auto index:indices) {
            const auto &pt=points_[index]; const QPointF q(X(pt.x),Y(pt.y));
            if(plotted_.isEmpty() || gap || pt.breakBefore) path.moveTo(q); else path.lineTo(q);
            plotted_.append(q); plottedIndices_.append(index);
        }
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
        const auto &pt=points_.at(plottedIndices_.at(best));
        QToolTip::showText(e->globalPosition().toPoint(), QStringLiteral("%1\n%2%3").arg(pt.label.toHtmlEscaped()).arg(pt.y,0,'g',10).arg(unit_.toHtmlEscaped()), this);
    } else QToolTip::hideText();
}
void LineChart::leaveEvent(QEvent *e) {
    hover_=-1; QToolTip::hideText(); update(); QWidget::leaveEvent(e);
}
void LineChart::contextMenuEvent(QContextMenuEvent *e) {
    QMenu menu(this);
    auto *exportAction=menu.addAction(QCoreApplication::translate("LineChart", "Export plotted samples…"));
    auto *exportVisible=menu.addAction(QCoreApplication::translate("LineChart", "Export visible samples…"));
    auto *table=menu.addAction(QCoreApplication::translate("LineChart", "View visible samples as table…"));
    table->setEnabled(begin_<end_);
    auto *reset=menu.addAction(QCoreApplication::translate("LineChart", "Reset zoom"));
    auto *zoom=menu.addAction(QCoreApplication::translate("LineChart", "Zoom in (Ctrl+Wheel)"));
    exportAction->setEnabled(!points_.isEmpty()); exportVisible->setEnabled(begin_<end_);
    reset->setEnabled(begin_!=0 || end_!=points_.size()); zoom->setEnabled(end_-begin_>2);
    const auto action=menu.exec(e->globalPos());
    if (action==table) {
        QDialog dialog(this); dialog.setWindowTitle(table->text()); dialog.resize(650,450);
        auto *layout=new QVBoxLayout(&dialog); auto *view=new QTableView(&dialog);
        view->setModel(new SampleModel(points_.mid(begin_,end_-begin_),unit_,view));
        view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        view->horizontalHeader()->setStretchLastSection(true); layout->addWidget(view);
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,&dialog);
        QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); layout->addWidget(buttons);
        dialog.exec(); return;
    }
    if (action==reset) { resetView(); return; }
    if (action==zoom) { const auto trim=std::max(qsizetype(1),(end_-begin_)/4); if(end_-begin_<=2) return; setViewRange(begin_+trim,end_-trim); return; }
    if (action!=exportAction && action!=exportVisible) return;
    const auto frozen=action==exportVisible?points_.mid(begin_,end_-begin_):points_;
    const QString path=QFileDialog::getSaveFileName(this,QCoreApplication::translate("LineChart", "Export plotted samples"),{},QStringLiteral("CSV (*.csv)"));
    if(path.isEmpty())return;
    // Export numeric samples only; labels can contain untrusted spreadsheet formula prefixes.
    QSaveFile f(path);
    if(!f.open(QIODevice::WriteOnly)) {QMessageBox::warning(this,QCoreApplication::translate("LineChart", "Export failed"),f.errorString());return;}
    QTextStream out(&f); out.setRealNumberPrecision(17); out << "x,y,break_before\n";
    for(const auto &pt:frozen) out<<pt.x<<','<<pt.y<<','<<(pt.breakBefore?1:0)<<'\n';
    out.flush();
    if(out.status()!=QTextStream::Ok||!f.commit())QMessageBox::warning(this,QCoreApplication::translate("LineChart", "Export failed"),f.errorString());
}

// ── BarChart ─────────────────────────────────────────────────────────────────

BarChart::BarChart(QWidget *parent) : QWidget(parent) { setAccessibleName(QCoreApplication::translate("BarChart", "Bar chart")); }

void BarChart::setBars(const QVector<Bar> &bars) {
    bars_ = bars;
    QStringList rows;
    for (const Bar &b : bars) rows << b.label + QStringLiteral(": ") + (std::isfinite(b.value)?b.text:QCoreApplication::translate("BarChart","Unavailable"));
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
    double minV = 0, maxV = 0;
    for (const Bar &b : bars_) {
        labelW = std::max(labelW, fm.horizontalAdvance(b.label));
        textW = std::max(textW, fm.horizontalAdvance(std::isfinite(b.value)?b.text:QCoreApplication::translate("BarChart","Unavailable")));
        if (std::isfinite(b.value)) { minV = std::min(minV,b.value); maxV = std::max(maxV,b.value); }
    }
    const int rowH = barRowHeight(fm);
    if (width() < 30) return;
    labelW = std::min(labelW + 10, width() / 2);
    textW = std::min(textW + 4, width() / 3);
    const int barMax = std::max(0, width() - labelW - textW - 8);
    const auto range = chartaxis::Range::make(minV,maxV);
    if (!range) return;
    const double zero = range->fraction(0);
    for (int i = 0; i < bars_.size(); ++i) {
        const Bar &b = bars_[i];
        const int y = i * rowH;
        p.setPen(colorFrom(theme::FG_DIM));
        p.drawText(QRect(0, y, labelW - 6, rowH), Qt::AlignRight | Qt::AlignVCenter, fm.elidedText(b.label, Qt::ElideRight, labelW - 6));
        const bool finite = std::isfinite(b.value);
        const double value = finite ? range->fraction(b.value) : zero;
        const int from = int(std::round(barMax * std::min(zero,value)));
        const int to = int(std::round(barMax * std::max(zero,value)));
        p.setPen(Qt::NoPen);
        p.setBrush(colorFrom(theme::ACCENT_SOFT));
        if (to > from) p.drawRoundedRect(QRect(labelW+from,y+4,to-from,rowH-8),2,2);
        if (minV < 0 && maxV > 0) {
            p.setPen(colorFrom(theme::FG_DIM));
            const int x=labelW+int(std::round(barMax*zero)); p.drawLine(x,y+2,x,y+rowH-2);
        }
        p.setPen(colorFrom(theme::MUTED));
        p.drawText(QRect(width() - textW, y, textW, rowH), Qt::AlignRight | Qt::AlignVCenter, fm.elidedText(finite ? b.text : QCoreApplication::translate("BarChart", "Unavailable"), Qt::ElideRight, textW));
    }
}

// ── DayBars ──────────────────────────────────────────────────────────────────

DayBars::DayBars(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Kernel events per day"));
}

void DayBars::setDays(const QVector<Day> &days) {
    days_ = days;
    QStringList rows;
    for (const Day &d : days) {
        qint64 n = 0;
        for (int c : d.kinds) n += std::max(0,c);
        if (n) rows << QStringLiteral("%1: %2").arg(d.date).arg(n);
    }
    setAccessibleDescription(rows.join(QStringLiteral("; ")));
    update();
}

void DayBars::setSelected(const QString &date) {
    selected_ = date;
    update();
}

QColor DayBars::colorOf(const QString &kind) {
    const char *const PALETTE[] = {theme::INFO, theme::WARN, theme::DANGER, theme::OK, theme::PURPLE,
                                          theme::ACCENT, theme::FG_DIM, theme::ACCENT_SOFT, theme::DANGER_SOFT};
    int sum = 0;  // not qHash: its seed changes between runs
    for (const QChar c : kind) sum += c.unicode();
    return colorFrom(PALETTE[sum % int(std::size(PALETTE))]);
}

QRectF DayBars::plotRect() const { return QRectF(rect()).adjusted(4, 4, -4, -fontMetrics().height()-5); }

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
    if (r.width()<=0 || r.height()<=0) return;
    qint64 maxTotal = 1;
    for (const Day &d : days_) {
        qint64 total = 0;
        for (int n : d.kinds) total += std::max(0,n);
        maxTotal = std::max(maxTotal, total);
    }
    const double colW = r.width() / days_.size();
    const double barW = std::max(0.0, colW * 0.7);
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
    const auto half=r.width()/2;
    const auto fm=fontMetrics();
    p.drawText(QRectF(r.left(),r.bottom()+2,half,fm.height()),Qt::AlignLeft,fm.elidedText(days_.first().date.mid(5),Qt::ElideRight,int(half)));
    p.drawText(QRectF(r.right()-half,r.bottom()+2,half,fm.height()),Qt::AlignRight,fm.elidedText(days_.last().date.mid(5),Qt::ElideLeft,int(half)));
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

void DayBars::keyPressEvent(QKeyEvent *e) {
    if (days_.isEmpty() || (e->key()!=Qt::Key_Left && e->key()!=Qt::Key_Right && e->key()!=Qt::Key_Home && e->key()!=Qt::Key_End)) {
        QWidget::keyPressEvent(e); return;
    }
    qsizetype i=0;
    while (i<days_.size() && days_[i].date!=selected_) ++i;
    if (e->key()==Qt::Key_Home) i=0;
    else if (e->key()==Qt::Key_End) i=days_.size()-1;
    else if (i==days_.size()) i=0;
    else i=std::clamp(i+(e->key()==Qt::Key_Left?-1:1),qsizetype(0),days_.size()-1);
    setSelected(days_[i].date); Q_EMIT dayClicked(selected_); e->accept();
}
