#pragma once
// Small painted charts for Health: the capacity line and the per-scene draw bars (Battery use), and the kernel
// events per day (Monitor). QPainter only, in the theme colours; they hold what they are given and nothing else.
#include <QColor>
#include <QMap>
#include <QVector>
#include <QWidget>

/// Line over x (any increasing number, e.g. a day number); `label` is the hover text, and the first and last
/// labels are printed under the axis.
class LineChart : public QWidget {
public:
    struct Point { double x; double y; QString label; };
    explicit LineChart(QWidget *parent = nullptr);
    void setPoints(const QVector<Point> &points, const QString &yUnit);

protected:
    void paintEvent(QPaintEvent *e) override;
    QSize minimumSizeHint() const override { return {240, 120}; }

private:
    QVector<Point> points_;
    QString unit_;
};

/// One horizontal bar per row, scaled to the largest value; `text` is printed after the bar.
class BarChart : public QWidget {
public:
    struct Bar { QString label; double value; QString text; };
    explicit BarChart(QWidget *parent = nullptr);
    void setBars(const QVector<Bar> &bars);

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QVector<Bar> bars_;
};

/// One column per day, stacked by event kind. A click reports the day; `setSelected` outlines it.
class DayBars : public QWidget {
    Q_OBJECT
public:
    struct Day { QString date; QMap<QString, int> kinds; };  // date "yyyy-MM-dd", oldest first
    explicit DayBars(QWidget *parent = nullptr);
    void setDays(const QVector<Day> &days);
    void setSelected(const QString &date);
    /// The segment colour of an event kind (the same kind has the same colour everywhere).
    static QColor colorOf(const QString &kind);

Q_SIGNALS:
    void dayClicked(const QString &date);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    QSize minimumSizeHint() const override { return {240, 90}; }

private:
    QRectF plotRect() const;
    int columnAt(const QPointF &p) const;

    QVector<Day> days_;
    QString selected_;
};
