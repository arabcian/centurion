#pragma once
// Small painted charts for Health: the capacity line and the per-scene draw bars (Battery use), and the kernel
// events per day (Monitor). QPainter only, in the theme colours; they hold what they are given and nothing else.
#include <QColor>
#include <QMap>
#include <QPair>
#include <QStringList>
#include <QVector>
#include <QWidget>

// Shared frozen presentation data; export fields from numericFrom onward must
// be numeric or empty, preceding text fields are spreadsheet-safe escaped.
namespace chartdata {
void table(QWidget *parent,const QStringList &headers,const QList<QStringList> &rows);
void exportRows(QWidget *parent,const QString &header,const QList<QStringList> &rows,int numericFrom);
}

/// Line over x (any increasing number, e.g. a day number); `label` is the hover text, and the first and last
/// labels are printed under the axis.
class LineChart : public QWidget {
public:
    struct Point { double x; double y; QString label; bool breakBefore = false; };
    explicit LineChart(QWidget *parent = nullptr);
    void setPoints(const QVector<Point> &points, const QString &yUnit);
    /// A view-only half-open sample range; invalid requests are clamped.
    void setViewRange(qsizetype begin, qsizetype end);
    QPair<qsizetype,qsizetype> viewRange() const { return {begin_,end_}; }
    void resetView() { setViewRange(0,points_.size()); }

protected:
    void paintEvent(QPaintEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    QSize minimumSizeHint() const override { return {240, 120}; }

private:
    QVector<Point> points_;
    QVector<QPointF> plotted_;
    QVector<qsizetype> plottedIndices_;
    qsizetype begin_ = 0, end_ = 0;
    int hover_ = -1;
    qsizetype keyboardSample_ = -1;
    QString unit_;
    void updateAccessibleDescription();
};

/// One horizontal bar per row, scaled to the largest value; `text` is printed after the bar.
class BarChart : public QWidget {
public:
    struct Bar { QString label; double value; QString text; };
    explicit BarChart(QWidget *parent = nullptr);
    void setBars(const QVector<Bar> &bars);
    void setViewRange(qsizetype begin, qsizetype end);
    QPair<qsizetype, qsizetype> viewRange() const { return {begin_, end_}; }
protected:
    void paintEvent(QPaintEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void changeEvent(QEvent *e) override;
private:
    QVector<Bar> bars_;
    qsizetype begin_ = 0, end_ = 0;
    double minV_ = 0, maxV_ = 0;
    QString rangeLabel() const;
    void refreshView();
};

/// One column per day, stacked by event kind. A click reports the day; `setSelected` outlines it.
class DayBars : public QWidget {
    Q_OBJECT
public:
    struct Day { QString date; QMap<QString, int> kinds; };  // date "yyyy-MM-dd", oldest first
    explicit DayBars(QWidget *parent = nullptr);
    void setDays(const QVector<Day> &days);
    void setSelected(const QString &date);
    void setViewRange(qsizetype begin, qsizetype end);
    QPair<qsizetype,qsizetype> viewRange() const {return {begin_,end_};}
    /// The segment colour of an event kind (the same kind has the same colour everywhere).
    static QColor colorOf(const QString &kind);

Q_SIGNALS:
    void dayClicked(const QString &date);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    QSize minimumSizeHint() const override { return {240, 90}; }

private:
    QRectF plotRect() const;
    int columnAt(const QPointF &p) const;

    QVector<Day> days_;
    qsizetype begin_=0,end_=0;
    QString selected_;
};
