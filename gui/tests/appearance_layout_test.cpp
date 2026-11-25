#include "../src/theme.h"
#include "../src/charts.h"
#include "../src/zone4canvas.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Failed: %s\n", #x); return 1; } } while (false)
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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
    chart.setPoints({{0,-std::numeric_limits<double>::max(),QStringLiteral("min")},{1,std::numeric_limits<double>::max(),QStringLiteral("max")}},QStringLiteral(" W"));
    CHECK(!chart.grab().isNull());
    chart.setPoints({{0,0,QStringLiteral("zero")},{1,std::numeric_limits<double>::denorm_min(),QStringLiteral("tiny")}},QStringLiteral(" W"));
    CHECK(!chart.grab().isNull());
    BarChart bar; bar.setBars({{QStringLiteral("negative"),-10,QStringLiteral("-10")},{QStringLiteral("positive"),5,QStringLiteral("5")},{QStringLiteral("missing"),std::numeric_limits<double>::quiet_NaN(),QStringLiteral("unknown")}});
    bar.resize(500,150); CHECK(!bar.grab().isNull());
    QVector<BarChart::Bar> many(100000, {QStringLiteral("sample"), 1.0, QStringLiteral("1")});
    bar.setBars(many); CHECK(bar.viewRange().first==0 && bar.viewRange().second==128);
    bar.setViewRange(100,200); CHECK(bar.viewRange().first==100 && bar.viewRange().second==200);
    bar.setViewRange(999999,999999); CHECK(bar.viewRange().first==99999 && bar.viewRange().second==100000);
    bar.setBars({}); CHECK(bar.viewRange().first==0 && bar.viewRange().second==0);

    const QString csvPath = tmp.filePath(QStringLiteral("samples.csv"));
    QTimer picker;
    QObject::connect(&picker, &QTimer::timeout, &app, [&] {
        auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        picker.stop();
        dialog->selectFile(csvPath);
        static_cast<QDialog *>(dialog)->accept();
    });
    QList<QStringList> csvRows(5000, {QStringLiteral("=text,\"quoted\""), QStringLiteral("-3.5")});
    picker.start(5);
    chartdata::exportRows(&bar, QStringLiteral("label,value"), csvRows, 1);
    QFile csv(csvPath);
    CHECK(csv.open(QIODevice::ReadOnly));
    CHECK(csv.readLine() == "label,value\n");
    for (qsizetype i = 0; i < csvRows.size(); ++i)
        CHECK(csv.readLine() == "\"'=text,\"\"quoted\"\"\",-3.5\n");
    CHECK(csv.atEnd());

    Zone4Canvas zone;zone.resize(400,120);zone.show();app.processEvents();
    int chosen=-1;zone.editZone=[&](int i){chosen=i;};
    CHECK(zone.hitZone({50,60})==0 && zone.hitZone({350,60})==3);
    zone.selectZone(2);zone.zoomBy(2.);zone.resize(640,160);app.processEvents();
    CHECK(zone.selectedZone()==2 && !zone.grab().isNull());
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QCoreApplication::sendEvent(&zone,&enter);CHECK(chosen==2);
    QKeyEvent left(QEvent::KeyPress,Qt::Key_Left,Qt::NoModifier);QCoreApplication::sendEvent(&zone,&left);CHECK(zone.selectedZone()==1);
    zone.fit();CHECK(zone.hitZone({80,80})==0 && zone.hitZone({560,80})==3);
    DayBars days;days.setDays({{QStringLiteral("2026-01-01"),{}},{QStringLiteral("2026-01-02"),{{QStringLiteral("Xid"),1}}},{QStringLiteral("2026-01-03"),{}}});
    days.setViewRange(1,3);CHECK(days.viewRange().first==1 && days.viewRange().second==3);
    QString selected;QObject::connect(&days,&DayBars::dayClicked,&days,[&](const QString &d){selected=d;});
    QKeyEvent home(QEvent::KeyPress,Qt::Key_Home,Qt::NoModifier);QCoreApplication::sendEvent(&days,&home);CHECK(selected==QStringLiteral("2026-01-02"));
    days.setViewRange(100,100);CHECK(days.viewRange().first==2 && days.viewRange().second==3);
    days.setDays({});CHECK(days.viewRange().first==0 && days.viewRange().second==0);
    return 0;
}
