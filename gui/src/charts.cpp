#include "charts.h"
#include "chartaxis.h"
#include <QAbstractTableModel>
#include <QVariant>
#include <utility>
#include <QDialog>
#include <QDialogButtonBox>
#include <QProgressBar>
#include <QTableView>
#include <QTimer>
#include <QStandardItemModel>
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

// Text fields from a log are never emitted as executable spreadsheet formulas.
QString csvText(QString value) {
    value.prepend(QLatin1Char('\''));
    value.replace(QLatin1Char('"'),QStringLiteral("\"\""));
    return QLatin1Char('"')+value+QLatin1Char('"');
}
} // namespace

namespace chartdata {
void table(QWidget *parent,const QStringList &headers,const QList<QStringList> &rows) {
    QDialog d(parent);d.setWindowTitle(QCoreApplication::translate("ChartData","Chart data"));d.resize(720,480);
    auto *layout=new QVBoxLayout(&d);auto *view=new QTableView;auto *model=new QStandardItemModel(view);
    model->setHorizontalHeaderLabels(headers);
    for(const auto &row:rows){QList<QStandardItem*> items;for(const auto &cell:row)items<<new QStandardItem(cell);model->appendRow(items);}
    view->setModel(model);view->setEditTriggers(QAbstractItemView::NoEditTriggers);view->horizontalHeader()->setStretchLastSection(true);layout->addWidget(view);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);QObject::connect(buttons,&QDialogButtonBox::rejected,&d,&QDialog::reject);layout->addWidget(buttons);d.exec();
}
void exportRows(QWidget *parent,const QString &header,const QList<QStringList> &rows,int numericFrom) {
    const auto path=QFileDialog::getSaveFileName(parent,QCoreApplication::translate("ChartData","Export chart data"),{},QStringLiteral("CSV (*.csv)"));if(path.isEmpty())return;
    QSaveFile f(path);if(!f.open(QIODevice::WriteOnly)){QMessageBox::warning(parent,QCoreApplication::translate("ChartData","Export failed"),f.errorString());return;}
    QTextStream out(&f);out<<header<<'\n';for(const auto &row:rows){for(int i=0;i<row.size();++i){if(i)out<<',';out<<(i<numericFrom?csvText(row[i]):row[i]);}out<<'\n';}out.flush();
    if(out.status()!=QTextStream::Ok||!f.commit())QMessageBox::warning(parent,QCoreApplication::translate("ChartData","Export failed"),f.errorString());
}

} // namespace chartdata

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
    unit_ = yUnit;
    resetView();
}

void LineChart::updateAccessibleDescription() {
    if (begin_ >= end_) {
        setAccessibleDescription(QCoreApplication::translate("LineChart", "No recorded samples."));
        return;
    }
    QString description = QCoreApplication::translate("LineChart", "%1 points, %2 to %3; last value %4 %5")
        .arg(end_ - begin_).arg(points_[begin_].label, points_[end_ - 1].label).arg(points_[end_ - 1].y).arg(unit_);
    if (keyboardSample_ >= begin_ && keyboardSample_ < end_) {
        const auto &sample = points_[keyboardSample_];
        description += QStringLiteral(" · ") + QCoreApplication::translate("LineChart", "Selected sample %1 of %2: %3, %4 %5")
            .arg(keyboardSample_ + 1).arg(points_.size()).arg(sample.label).arg(sample.y).arg(unit_);
    }
    setAccessibleDescription(description);
}

void LineChart::setViewRange(qsizetype begin, qsizetype end) {
    begin_ = std::clamp(begin, qsizetype(0), points_.size());
    end_ = std::clamp(end, begin_, points_.size());
    if (begin_ == end_ && !points_.isEmpty()) { begin_ = std::min(begin_,points_.size()-1); end_ = begin_+1; }
    plotted_.clear();
    plottedIndices_.clear();
    hover_ = -1;
    if (keyboardSample_ >= 0) QToolTip::hideText();
    keyboardSample_ = -1;
    updateAccessibleDescription();
    update();
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
    if (e->key() == Qt::Key_Escape && keyboardSample_ >= 0) {
        keyboardSample_ = -1;
        QToolTip::hideText();
        updateAccessibleDescription();
        update();
        e->accept();
        return;
    }
    if (begin_ < end_ && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down)) {
        const auto step = e->key() == Qt::Key_Up ? -1 : 1;
        if (keyboardSample_ < 0) {
            keyboardSample_ = hover_ >= 0 && hover_ < plottedIndices_.size() ? plottedIndices_.at(hover_) : begin_;
        } else {
            keyboardSample_ = std::clamp(keyboardSample_ + step, begin_, end_ - 1);
        }
        hover_ = -1;
        const auto &sample = points_[keyboardSample_];
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 0)),
                           QStringLiteral("%1: %2 %3").arg(sample.label.toHtmlEscaped()).arg(sample.y, 0, 'g', 10)
                               .arg(unit_.toHtmlEscaped()), this);
        updateAccessibleDescription();
        update();
        e->accept();
        return;
    }
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
    if ((keyboardSample_ >= begin_ && keyboardSample_ < end_) || (hover_ >= 0 && hover_ < plotted_.size())) {
        const auto pt = keyboardSample_ >= begin_ && keyboardSample_ < end_
            ? QPointF(X(points_[keyboardSample_].x), Y(points_[keyboardSample_].y))
            : plotted_.at(hover_);
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
    if (keyboardSample_ >= 0) {
        keyboardSample_ = -1;
        updateAccessibleDescription();
        update();
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
    const auto frozen = points_;
    const qsizetype first = action == exportVisible ? begin_ : 0;
    const qsizetype last = action == exportVisible ? end_ : frozen.size();
    const QString title = QCoreApplication::translate("LineChart", "Export plotted samples");
    const QString path=QFileDialog::getSaveFileName(this,title,{},QStringLiteral("CSV (*.csv)"));
    if(path.isEmpty())return;
    // Export numeric samples only; labels can contain untrusted spreadsheet formula prefixes.
    QSaveFile f(path);
    if(!f.open(QIODevice::WriteOnly)) {QMessageBox::warning(this,QCoreApplication::translate("LineChart", "Export failed"),f.errorString());return;}
    QTextStream out(&f); out.setRealNumberPrecision(17); out << "x,y,break_before\n";
    qsizetype index = first;
    constexpr qsizetype batchSize = 4096;
    const auto writeBatch = [&] {
        const auto stop = index + std::min(batchSize, last - index);
        for (; index < stop && out.status() == QTextStream::Ok; ++index) {
            const auto &pt = frozen.at(index);
            out << pt.x << ',' << pt.y << ',' << (pt.breakBefore ? 1 : 0) << '\n';
        }
    };
    if (last - first <= batchSize) {
        writeBatch();
    } else {
        QDialog progress(this);
        progress.setWindowTitle(title);
        progress.setMinimumWidth(320);
        auto *layout = new QVBoxLayout(&progress);
        auto *progressBar = new QProgressBar;
        progressBar->setRange(0, 1000);
        progressBar->setValue(0);
        layout->addWidget(progressBar);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &progress, &QDialog::reject);
        QTimer timer;
        connect(&timer, &QTimer::timeout, &progress, [&] {
            writeBatch();
            progressBar->setValue(int(1000.0 * double(index - first) / double(last - first)));
            if (index == last || out.status() != QTextStream::Ok) {
                timer.stop();
                progress.accept();
            }
        });
        timer.start(0);
        if (progress.exec() != QDialog::Accepted) return;
    }
    out.flush();
    if(out.status()!=QTextStream::Ok||!f.commit())QMessageBox::warning(this,QCoreApplication::translate("LineChart", "Export failed"),f.errorString());
}

// ── BarChart ─────────────────────────────────────────────────────────────────

BarChart::BarChart(QWidget *parent) : QWidget(parent) {
    setAccessibleName(QCoreApplication::translate("BarChart", "Bar chart"));
    setFocusPolicy(Qt::StrongFocus);
}

void BarChart::setBars(const QVector<Bar> &bars) {
    bars_ = bars;
    minV_ = maxV_ = 0;
    for (const auto &b : bars_) if (std::isfinite(b.value)) { minV_ = std::min(minV_,b.value); maxV_ = std::max(maxV_,b.value); }
    setViewRange(0,bars_.size());
}

void BarChart::setViewRange(qsizetype begin, qsizetype end) {
    const auto n = bars_.size();
    if (!n) { begin_ = end_ = 0; }
    else {
        begin_ = std::clamp<qsizetype>(begin,0,n-1);
        end_ = std::clamp<qsizetype>(end,begin_+1,n);
        end_ = begin_ + std::min<qsizetype>(128,end_-begin_);
    }
    refreshView();
}

QString BarChart::rangeLabel() const {
    if (bars_.isEmpty()) return {};
    return QCoreApplication::translate("BarChart", "Rows %1–%2 of %3")
        .arg(begin_ + 1).arg(end_).arg(bars_.size());
}

void BarChart::refreshView() {
    QStringList rows;
    for (qsizetype i=begin_; i<end_; ++i) {
        const auto &b=bars_.at(i);
        rows << b.label + QStringLiteral(": ") + (std::isfinite(b.value)?b.text:QCoreApplication::translate("BarChart","Unavailable"));
    }
    if (!bars_.isEmpty()) rows.prepend(rangeLabel());
    setAccessibleDescription(rows.join(QStringLiteral("; ")));
    const int footer = bars_.isEmpty() ? 0 : fontMetrics().height() + 8;
    setMinimumHeight(int(end_-begin_) * barRowHeight(fontMetrics()) + footer + 4);
    update();
}

void BarChart::changeEvent(QEvent *e) {
    QWidget::changeEvent(e);
    if (e->type()==QEvent::FontChange) refreshView();
}

void BarChart::keyPressEvent(QKeyEvent *e) {
    if (bars_.isEmpty()) { QWidget::keyPressEvent(e); return; }
    const auto count=end_-begin_;
    if (e->key()==Qt::Key_Home) setViewRange(0,bars_.size());
    else if (e->key()==Qt::Key_End) setViewRange(bars_.size() - count, bars_.size());
    else if (e->key()==Qt::Key_PageUp) { const auto b=std::max<qsizetype>(0,begin_-count); setViewRange(b,b+count); }
    else if (e->key()==Qt::Key_PageDown) { const auto b=std::min(begin_+count,bars_.size()-count); setViewRange(b,b+count); }
    else { QWidget::keyPressEvent(e); return; }
    e->accept();
}

void BarChart::wheelEvent(QWheelEvent *e) {
    if (!(e->modifiers() & Qt::ControlModifier) || !e->angleDelta().y() || bars_.isEmpty()) { QWidget::wheelEvent(e); return; }
    const auto count=end_-begin_;
    const auto wanted=e->angleDelta().y()>0?std::max<qsizetype>(1,count/2):std::min<qsizetype>(128,count*2);
    setViewRange(begin_,begin_+std::min(wanted,bars_.size()-begin_));
    e->accept();
}

void BarChart::paintEvent(QPaintEvent *) {
    if (bars_.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(font());
    int labelW = 0, textW = 0;
    const double minV = minV_, maxV = maxV_;
    for (qsizetype i=begin_; i<end_; ++i) {
        const auto &b=bars_.at(i);
        labelW = std::max(labelW, fm.horizontalAdvance(b.label));
        textW = std::max(textW, fm.horizontalAdvance(std::isfinite(b.value)?b.text:QCoreApplication::translate("BarChart","Unavailable")));
    }
    const int rowH = barRowHeight(fm);
    if (width() < 30) return;
    labelW = std::min(labelW + 10, width() / 2);
    textW = std::min(textW + 4, width() / 3);
    const int barMax = std::max(0, width() - labelW - textW - 8);
    const auto range = chartaxis::Range::make(minV,maxV);
    if (!range) return;
    const double zero = range->fraction(0);
    for (qsizetype i = begin_; i < end_; ++i) {
        const Bar &b = bars_[i];
        const int y = int(i-begin_) * rowH;
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
    p.setPen(colorFrom(theme::FG_DIM));
    const QRect footer(0, int(end_ - begin_) * rowH + 4, width(), fm.height() + 4);
    p.drawText(footer, Qt::AlignCenter, fm.elidedText(rangeLabel(), Qt::ElideRight, footer.width()));
}

void BarChart::contextMenuEvent(QContextMenuEvent *e) {
    if(bars_.isEmpty())return;
    const auto frozen=bars_, visible=bars_.mid(begin_,end_-begin_);
    QMenu m(this);
    auto *table=m.addAction(QCoreApplication::translate("ChartData","View visible data table"));
    auto *csv=m.addAction(QCoreApplication::translate("ChartData","Export all data"));
    auto *csvVisible=m.addAction(QCoreApplication::translate("ChartData","Export visible data"));
    auto *previous=m.addAction(QCoreApplication::translate("BarChart","Previous rows")); previous->setEnabled(begin_>0);
    auto *next=m.addAction(QCoreApplication::translate("BarChart","Next rows")); next->setEnabled(end_<bars_.size());
    auto *reset=m.addAction(QCoreApplication::translate("BarChart","Reset view"));
    const auto action=m.exec(e->globalPos());if(!action)return;
    const auto count=end_-begin_;
    if(action==previous) { const auto b=std::max<qsizetype>(0,begin_-count); setViewRange(b,b+count); return; }
    if(action==next) { const auto b=std::min(begin_+count,bars_.size()-count); setViewRange(b,b+count); return; }
    if(action==reset) {setViewRange(0,bars_.size());return;}
    QList<QStringList> rows;
    for(const auto &b:(action==csv?frozen:visible))rows.append(QStringList{b.label,std::isfinite(b.value)?b.text:QCoreApplication::translate("BarChart","Unavailable"),std::isfinite(b.value)?QString::number(b.value,'g',17):QString()});
    if(action==table)chartdata::table(this,{QCoreApplication::translate("ChartData","Label"),QCoreApplication::translate("ChartData","Display value"),QCoreApplication::translate("ChartData","Numeric value")},rows);
    else if(action==csv||action==csvVisible)chartdata::exportRows(this,QStringLiteral("label,display_value,numeric_value"),rows,2);
}

// ── DayBars ──────────────────────────────────────────────────────────────────

DayBars::DayBars(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Kernel events per day"));
}

void DayBars::setDays(const QVector<Day> &days) {
    days_ = days;begin_=0;end_=days_.size();
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
    if(end_<=begin_)return -1;
    return int(std::min(end_-1,begin_+qsizetype((p.x()-r.left())/(r.width()/(end_-begin_)))));
}

void DayBars::paintEvent(QPaintEvent *) {
    if (days_.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    if (r.width()<=0 || r.height()<=0) return;
    qint64 maxTotal = 1;
    for(qsizetype i=begin_;i<end_;++i) {
        const Day &d=days_.at(i);
        qint64 total = 0;
        for (int n : d.kinds) total += std::max(0,n);
        maxTotal = std::max(maxTotal, total);
    }
    if(end_<=begin_)return;
    const double colW = r.width() / (end_-begin_);
    const double barW = std::max(0.0, colW * 0.7);
    for (qsizetype i=begin_;i<end_;++i) {
        const Day &d = days_[i];
        const double x = r.left() + (i-begin_) * colW + (colW - barW) / 2;
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
            p.drawRect(QRectF(r.left() + (i-begin_) * colW, r.top(), colW, r.height()));
        }
    }
    p.setPen(QPen(colorFrom(theme::BORDER_SOFT)));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.setPen(colorFrom(theme::MUTED));
    const auto half=r.width()/2;
    const auto fm=fontMetrics();
    p.drawText(QRectF(r.left(),r.bottom()+2,half,fm.height()),Qt::AlignLeft,fm.elidedText(days_.at(begin_).date.mid(5),Qt::ElideRight,int(half)));
    p.drawText(QRectF(r.right()-half,r.bottom()+2,half,fm.height()),Qt::AlignRight,fm.elidedText(days_.at(end_-1).date.mid(5),Qt::ElideLeft,int(half)));
}

void DayBars::mousePressEvent(QMouseEvent *e) {
    const int i = columnAt(e->position());
    if (e->button()==Qt::LeftButton && i >= 0) Q_EMIT dayClicked(days_[i].date);
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
    if(end_<=begin_)return;
    qsizetype i=begin_;
    while (i<end_ && days_[i].date!=selected_) ++i;
    if (e->key()==Qt::Key_Home) i=begin_;
    else if (e->key()==Qt::Key_End) i=end_-1;
    else if (i==end_) i=begin_;
    else i=std::clamp(i+(e->key()==Qt::Key_Left?-1:1),begin_,end_-1);
    setSelected(days_[i].date); Q_EMIT dayClicked(selected_); e->accept();
}

void DayBars::setViewRange(qsizetype begin,qsizetype end) {
    begin_=std::clamp(begin,qsizetype(0),days_.size());end_=std::clamp(end,begin_,days_.size());
    if(begin_==end_&&!days_.isEmpty()){begin_=std::min(begin_,days_.size()-1);end_=begin_+1;}update();
}
void DayBars::wheelEvent(QWheelEvent *e) {
    if(!(e->modifiers()&Qt::ControlModifier)||days_.isEmpty()||!e->angleDelta().y()){e->ignore();return;}
    const auto count=end_-begin_;const auto length=std::clamp(e->angleDelta().y()>0?count*3/4:count+std::max(qsizetype(1),count/3),qsizetype(1),days_.size());
    const auto first=std::clamp(begin_+(count-length)/2,qsizetype(0),days_.size()-length);setViewRange(first,first+length);e->accept();
}
void DayBars::contextMenuEvent(QContextMenuEvent *e) {
    if(days_.isEmpty())return;
    const auto all=days_,visible=days_.mid(begin_,end_-begin_);
    QMenu m(this);auto *reset=m.addAction(tr("Show all days"));auto *left=m.addAction(tr("Earlier days"));auto *right=m.addAction(tr("Later days"));m.addSeparator();auto *table=m.addAction(QCoreApplication::translate("ChartData","View data table"));auto *csv=m.addAction(QCoreApplication::translate("ChartData","Export all data"));auto *current=m.addAction(QCoreApplication::translate("ChartData","Export visible data"));
    const auto action=m.exec(e->globalPos());if(!action)return;
    if(action==reset){setViewRange(0,days_.size());return;}
    if(action==left||action==right){const auto count=end_-begin_;const auto start=std::clamp(begin_+(action==left?-1:1)*std::max(qsizetype(1),count/2),qsizetype(0),days_.size()-count);setViewRange(start,start+count);return;}
    QList<QStringList> rows;for(const auto &d:(action==csv?all:visible)){if(d.kinds.isEmpty())rows.append(QStringList{d.date,QString(),QString()});else for(auto i=d.kinds.cbegin();i!=d.kinds.cend();++i)rows.append(QStringList{d.date,i.key(),QString::number(i.value())});}
    if(action==table)chartdata::table(this,{tr("Date"),tr("Event kind"),tr("Count")},rows);else if(action==csv||action==current)chartdata::exportRows(this,QStringLiteral("date,event_kind,count"),rows,2);
}
