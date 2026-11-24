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
    return 0;
}
