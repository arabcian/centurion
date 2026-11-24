#include "../src/theme.h"
#include "../src/charts.h"
#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <cstdio>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Failed: %s\n", #x); return 1; } } while (false)
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp; CHECK(tmp.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
    qunsetenv("CENTURION_UI");
    for (bool mode : {false,true}) for (bool compact : {false,true}) {
        CHECK(theme::saveUiMode(mode)); CHECK(theme::saveCompactDensity(compact));
        for (const auto &[id,name] : theme::themes()) {
            CHECK(theme::saveTheme(id)); theme::load(); theme::apply(app);
            QWidget root;
            auto *layout=new QVBoxLayout(&root);
            auto *box=new QGroupBox(name); auto *controls=new QVBoxLayout(box);
            auto *button=new QPushButton(QStringLiteral("Apply selection / Seçimi uygula")); controls->addWidget(button);
            auto *combo=new QComboBox; combo->addItems({QStringLiteral("Balanced Performance"),QStringLiteral("Custom")}); controls->addWidget(combo);
            auto *edit=new QLineEdit(QStringLiteral("4096 MiB")); controls->addWidget(edit); layout->addWidget(box);
            auto *chart=new LineChart; chart->setPoints({{0,5,QStringLiteral("A")},{1,40,QStringLiteral("B")},{2,8,QStringLiteral("C")}},QStringLiteral(" ms")); layout->addWidget(chart,1);
            for (const QSize size : {QSize(360,420),QSize(800,500)}) {
                root.resize(size); root.show(); app.processEvents();
                CHECK(button->width()>=button->minimumSizeHint().width());
                CHECK(combo->width()>=combo->minimumSizeHint().width());
                CHECK(chart->width()>0 && chart->height()>0);
                const QImage frame=root.grab().toImage(); CHECK(!frame.isNull());
                root.hide();
            }
        }
    }
    LineChart chart; QVector<LineChart::Point> samples;
    for (int i=0;i<100000;++i) samples.append({double(i),i==50000?100000.0:double(i%10),QString::number(i)});
    samples[10].y=std::numeric_limits<double>::quiet_NaN();
    chart.setPoints(samples,QStringLiteral("ms"));
    CHECK(chart.viewRange().second==99999);
    chart.setViewRange(200,400); CHECK(chart.viewRange().first==200 && chart.viewRange().second==400);
    chart.setViewRange(-20,200000); CHECK(chart.viewRange().first==0 && chart.viewRange().second==99999);
    chart.resize(800,500); chart.show(); app.processEvents(); CHECK(!chart.grab().isNull());
    chart.setViewRange(999999,999999); CHECK(chart.viewRange().second-chart.viewRange().first==1);
    chart.resetView(); CHECK(chart.viewRange().second==99999);
    return 0;
}
