#include "simulation.h"
#include "charts.h"
#include "theme.h"
#include "vfcurvewidget.h"
#include "systemdiagnostics.h"
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTemporaryDir>
#include <memory>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>
#include <array>
#include <limits>

namespace simulation {
namespace {
struct Simulation { Q_DECLARE_TR_FUNCTIONS(Simulation) };
struct Fixture { int cpu, gpu; bool nvidia, available; };
constexpr std::array<Fixture,6> fixtures{{
    {68, 72, true, true},
    {63, 65, true, true},
    {51, -1, false, true},
    {61, 69, false, true},
    {-1, -1, false, false},
    {47, -1, true, true},
}};
// Generated paths only: no user-supplied root, symlinks, commands or host reads.
bool fixtureFile(const QString &root, const QString &path, const QByteArray &bytes) {
    if (root.isEmpty()) return false;
    const QString full=root+path;
    if (!QDir().mkpath(QFileInfo(full).absolutePath())) return false;
    QFile file(full);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.flush();
}
bool buildFixture(const QString &root, int index) {
    if (root.isEmpty() || !QDir().mkpath(root)) return false;
    if (index==4) return true; // intentionally unavailable sysfs/procfs
    const QByteArray available=index==3 ? "2097152" : "33554432";
    if (!fixtureFile(root,QStringLiteral("/proc/meminfo"),QByteArray("MemTotal: 67108864 kB\nMemAvailable: ")+available+" kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n")) return false;
    if (!fixtureFile(root,QStringLiteral("/proc/pressure/memory"),"some avg60=0.10\nfull avg60=0.00\n")) return false;
    const QString bat=QStringLiteral("/sys/class/power_supply/BAT0/");
    const QString ac=QStringLiteral("/sys/class/power_supply/AC/");
    return fixtureFile(root,bat+QStringLiteral("type"),"Battery\n") &&
           fixtureFile(root,bat+QStringLiteral("status"),index==5?"Discharging\n":"Charging\n") &&
           fixtureFile(root,bat+QStringLiteral("capacity"),"65\n") &&
           fixtureFile(root,bat+QStringLiteral("energy_full"),"80000000\n") &&
           fixtureFile(root,bat+QStringLiteral("energy_full_design"),"90000000\n") &&
           fixtureFile(root,ac+QStringLiteral("type"),"Mains\n") &&
           fixtureFile(root,ac+QStringLiteral("online"),index==5?"0\n":"1\n");
}
QString reading(int v) { return v < 0 ? Simulation::tr("Unavailable") : QStringLiteral("%1 °C").arg(v); }
}
QWidget *makeWorkspace(QApplication &app) {
    auto *root = new QWidget;
    root->setWindowTitle(Simulation::tr("Centurion — Hardware Simulation"));
    auto *layout = new QVBoxLayout(root);
    auto *notice = new QLabel(Simulation::tr("SIMULATION — fixture data only. No hardware, helper, daemon or real profile access."));
    notice->setWordWrap(true); notice->setProperty("role", "title"); layout->addWidget(notice);
    auto *form = new QFormLayout;
    auto *profile = new QComboBox; profile->setObjectName(QStringLiteral("fixturePicker"));
    profile->addItems({Simulation::tr("AMD dual-CCD / NVIDIA"), Simulation::tr("Intel hybrid / NVIDIA"), Simulation::tr("Intel non-hybrid / no GPU"), Simulation::tr("AMD / AMD GPU"), Simulation::tr("Missing sensors"), Simulation::tr("Sleeping NVIDIA GPU")});
    form->addRow(Simulation::tr("Fixture"), profile);
    auto *palette = new QComboBox; palette->setObjectName(QStringLiteral("fixtureTheme"));
    for (const auto &[id,name] : theme::themes()) palette->addItem(theme::themePreviewIcon(id),name,id);
    palette->setCurrentIndex(palette->findData(theme::currentTheme()));
    form->addRow(Simulation::tr("Live theme"),palette); layout->addLayout(form);
    auto *tabs=new QTabWidget; tabs->setObjectName(QStringLiteral("fixtureTabs")); layout->addWidget(tabs,1);
    auto *metrics=new QWidget; auto *metricLayout=new QVBoxLayout(metrics);
    tabs->addTab(metrics,Simulation::tr("Telemetry and Curve"));
    auto *diagnostics=new QPlainTextEdit; diagnostics->setReadOnly(true); diagnostics->setObjectName(QStringLiteral("fixtureDiagnostics"));
    tabs->addTab(diagnostics,Simulation::tr("Read-only Diagnostics"));
    // A fresh private directory for these actual diagnostic parsers. Failure
    // never falls back to an empty root (which would mean the live host).
    const auto fakeRoot=std::make_shared<QTemporaryDir>();
    auto *sensor = new QLabel; sensor->setWordWrap(true); sensor->setObjectName(QStringLiteral("fixtureSensors")); metricLayout->addWidget(sensor);
    auto *chart = new LineChart; chart->setObjectName(QStringLiteral("fixtureChart")); chart->setContextMenuPolicy(Qt::PreventContextMenu); metricLayout->addWidget(chart,1);
    auto *curve = new VfCurveWidget; curve->setObjectName(QStringLiteral("fixtureCurve")); curve->setContextMenuPolicy(Qt::PreventContextMenu); metricLayout->addWidget(curve,2);
    auto *edit = new QSpinBox; edit->setObjectName(QStringLiteral("fixtureDraft")); edit->setRange(-100,100); edit->setSuffix(QStringLiteral(" MHz"));
    auto *editForm = new QFormLayout; editForm->addRow(Simulation::tr("Simulated draft offset"),edit); metricLayout->addLayout(editForm);
    auto *apply = new QPushButton(Simulation::tr("Apply to simulated memory only")); apply->setObjectName(QStringLiteral("fixtureApply")); metricLayout->addWidget(apply);
    auto *result = new QLabel; result->setObjectName(QStringLiteral("fixtureResult")); result->setWordWrap(true); metricLayout->addWidget(result);
    const QVector<QPointF> base{{700,1200},{750,1500},{800,1800},{850,2000},{900,2200}};
    auto updateCurve = [curve,edit,base]{
        auto points=base; for (auto &point : points) point.setY(point.y()+edit->value());
        curve->setPoints(points);
    };
    QObject::connect(edit,&QSpinBox::valueChanged,root,[=](int){updateCurve();result->clear();});
    QObject::connect(curve,&VfCurveWidget::selectionShifted,root,[=](int delta){edit->setValue(edit->value()+delta);});
    QObject::connect(curve,&VfCurveWidget::pointEdited,root,[=](int index,int frequency){
        // Fixture intentionally models one global offset, not per-point hardware edits.
        if(index>=0 && index<base.size()) {edit->setValue(frequency-int(base[index].y()));updateCurve();}
    });
    QObject::connect(apply,&QPushButton::clicked,root,[=]{
        curve->setReferenceCurves(base,curve->points());
        result->setText(Simulation::tr("Simulated readback: %1 MHz offset. No hardware write.").arg(edit->value()));
    });
    auto loadFixture = [=](int index){
        if(index<0 || index>=int(fixtures.size())) return;
        const auto &f=fixtures[std::size_t(index)];
        if(fakeRoot->isValid()) {
            const QString path=fakeRoot->filePath(QString::number(index));
            if (buildFixture(path,index))
                diagnostics->setPlainText(systemdiagnostics::memory(path).toText()+QStringLiteral("\n")+systemdiagnostics::charging(path).toText());
            else diagnostics->setPlainText(Simulation::tr("Could not create the isolated fixture."));
        } else diagnostics->setPlainText(Simulation::tr("Could not create the isolated fixture."));
        sensor->setText(Simulation::tr("CPU: %1 · GPU: %2").arg(reading(f.cpu),reading(f.gpu)));
        QVector<LineChart::Point> history;
        const double missing=std::numeric_limits<double>::quiet_NaN();
        for(int i=0;i<8;++i) history.append({double(i),f.cpu<0 || i==3 ? missing : double(f.cpu+i%3),QString::number(i)});
        chart->setPoints(history,QStringLiteral("°C"));
        edit->setValue(0);result->clear();
        curve->clearSelection();curve->setReferenceCurves(base,{});updateCurve();curve->fitAxes();
        curve->setVisible(f.nvidia); edit->setEnabled(f.nvidia && f.gpu>=0 && f.available);
        curve->setEnabled(edit->isEnabled());apply->setEnabled(edit->isEnabled());
    };
    QObject::connect(profile,&QComboBox::currentIndexChanged,root,loadFixture);
    QObject::connect(palette,&QComboBox::activated,root,[palette,&app](int i){
        if(!theme::applyLivePalette(app,palette->itemData(i).toString())) palette->setCurrentIndex(palette->findData(theme::currentTheme()));
    });
    QObject::connect(&theme::events(),&theme::ThemeEvents::themeChanged,root,[palette](quint64){
        palette->setCurrentIndex(palette->findData(theme::currentTheme()));
    });
    loadFixture(0);
    root->resize(900,650);
    return root;
}
int run(QApplication &app) {
    theme::apply(app);
    auto *root=makeWorkspace(app);
    root->show();
    const int rc=app.exec(); delete root; return rc;
}
} // namespace simulation
