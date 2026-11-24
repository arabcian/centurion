#include "../src/simulation.h"
#include "../src/theme.h"
#include "../src/vfcurvewidget.h"
#include "../src/charts.h"
#include "../src/i18n.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTranslator>
#include <cstdlib>
#include <QWidget>
#include <algorithm>
#include <cstdio>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"Failed line %d: %s\n",__LINE__,#x); return 1; } } while(false)

// Return the fraction of pixels with any RGB component changing by more than
// 12/255. Dimensions must match; a diff image highlights the affected pixels.
static double difference(const QImage &a0,const QImage &b0,QImage *diff=nullptr) {
    if(a0.isNull() || b0.isNull() || a0.size()!=b0.size()) return 1.;
    const auto a=a0.convertToFormat(QImage::Format_RGB32), b=b0.convertToFormat(QImage::Format_RGB32);
    if(diff) *diff=QImage(a.size(),QImage::Format_RGB32);
    qint64 changed=0;
    for(int y=0;y<a.height();++y) {
        const auto *ar=reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto *br=reinterpret_cast<const QRgb*>(b.constScanLine(y));
        auto *dr=diff ? reinterpret_cast<QRgb*>(diff->scanLine(y)) : nullptr;
        for(int x=0;x<a.width();++x) {
            const int delta=std::max({std::abs(qRed(ar[x])-qRed(br[x])),std::abs(qGreen(ar[x])-qGreen(br[x])),std::abs(qBlue(ar[x])-qBlue(br[x]))});
            const bool hit=delta>12; changed+=hit;
            if(dr) dr[x]=hit?qRgb(255,0,255):qRgb(0,0,0);
        }
    }
    return double(changed)/(qint64(a.width())*a.height());
}
int main(int argc,char **argv) {
    QTemporaryDir isolated; CHECK(isolated.isValid());
    for(const char *var:{"XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_DATA_HOME","XDG_STATE_HOME","XDG_RUNTIME_DIR"}) qputenv(var,isolated.path().toUtf8());
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,isolated.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,isolated.path());
    QApplication app(argc,argv); app.setFont(QFont(QStringLiteral("DejaVu Sans"),9));
    qunsetenv("CENTURION_UI");
    i18n::install(app);
    const QString locale=i18n::effective();
#ifdef CENTURION_TEST_QM
    QTranslator own;
    if (locale==QStringLiteral("tr")) { CHECK(own.load(QStringLiteral(CENTURION_TEST_QM))); app.installTranslator(&own); }
#endif
    const QString scale=qEnvironmentVariable("QT_SCALE_FACTOR", "1");
    const QString mode=qEnvironmentVariable("CENTURION_VISUAL_MODE", "experimental");
    const QString variant=QStringLiteral("%1-%2-%3").arg(mode,locale,scale);
    const QString output=QDir::current().filePath(QStringLiteral("visual-artifacts/")+variant);
    const bool fullMatrix=locale==QStringLiteral("en") && scale==QStringLiteral("1") && mode==QStringLiteral("experimental");
    CHECK(QDir().mkpath(output));
    const QString baseline=qEnvironmentVariable("CENTURION_VISUAL_BASELINES");
    const QStringList ids{QStringLiteral("graphite"),QStringLiteral("pastel-mint-cream"),QStringLiteral("oled-black"),QStringLiteral("gruvbox")};
    CHECK(theme::saveUiMode(mode==QStringLiteral("experimental"))); CHECK(theme::saveCompactDensity(true));
    CHECK(theme::saveTheme(ids.first())); theme::load();theme::apply(app);
    std::unique_ptr<QWidget> root(simulation::makeWorkspace(app));
    root->show();app.processEvents();
    auto *picker=root->findChild<QComboBox*>(QStringLiteral("fixturePicker"));
    auto *draft=root->findChild<QSpinBox*>(QStringLiteral("fixtureDraft"));
    auto *curve=root->findChild<VfCurveWidget*>(QStringLiteral("fixtureCurve"));
    auto *apply=root->findChild<QPushButton*>(QStringLiteral("fixtureApply"));
    auto *result=root->findChild<QLabel*>(QStringLiteral("fixtureResult"));
    auto *tabs=root->findChild<QTabWidget*>(QStringLiteral("fixtureTabs"));
    auto *diagnostics=root->findChild<QPlainTextEdit*>(QStringLiteral("fixtureDiagnostics"));
    CHECK(picker && draft && curve && apply && result && tabs && diagnostics);
    CHECK(diagnostics->toPlainText().contains(QStringLiteral("S10")) && diagnostics->toPlainText().contains(QStringLiteral("S13")));
    draft->setValue(-45);apply->click();curve->selectAll();curve->setZoom(2.);
    const auto points=curve->points();const auto selection=curve->selection();const auto readback=result->text();
    const char *stable=theme::FG;const auto initialRevision=theme::revision();
    quint64 signaled=0;QObject::connect(&theme::events(),&theme::ThemeEvents::themeChanged,root.get(),[&](quint64 rev){signaled=rev;});
    for(int i=0;i<20;++i){
        CHECK(theme::applyLivePalette(app,ids[i%ids.size()]));app.processEvents();
        CHECK(stable==theme::FG && QByteArray(stable)==theme::paletteColors(ids[i%ids.size()])[8].toLatin1());
        CHECK(draft->value()==-45 && curve->points()==points && curve->selection()==selection && curve->zoom()==2. && result->text()==readback);
        CHECK(signaled==initialRevision+quint64(i)+1);
    }
    CHECK(!theme::applyLivePalette(app,QStringLiteral("no-such-palette")) && theme::revision()==initialRevision+20);
    CHECK(!theme::restartRequested());
    tabs->setCurrentIndex(1);
    const auto diagnosticText=diagnostics->toPlainText();
    CHECK(theme::applyLivePalette(app,ids.first()) && tabs->currentIndex()==1 && diagnostics->toPlainText()==diagnosticText);
    tabs->setCurrentIndex(0);
    const auto custom=theme::paletteColors(QStringLiteral("gruvbox"));
    CHECK(theme::saveCustomPalette(custom));
    CHECK(theme::applyLivePalette(app,QStringLiteral("custom")));
    CHECK(stable==theme::FG && QByteArray(stable)==custom[8].toLatin1());
    CHECK(!theme::saveCustomPalette({QStringLiteral("invalid")}));
    CHECK(theme::currentTheme()==QStringLiteral("custom") && draft->value()==-45);
    // Rendering tests use a fixed combination of fixture, geometry and palette.
    // Baseline files are optional, explicitly supplied, and never updated here.
    for(int i=0;i<picker->count();++i){
        picker->setCurrentIndex((i+1)%picker->count()); picker->setCurrentIndex(i);app.processEvents();
        CHECK(apply->isEnabled()==(i==0 || i==1));
        CHECK(draft->isEnabled()==apply->isEnabled());
        CHECK(diagnostics->toPlainText().contains(QStringLiteral("S10")) && diagnostics->toPlainText().contains(QStringLiteral("S13")));
        if(i==4) CHECK(!diagnostics->toPlainText().contains(QStringLiteral("64 GiB")));
        // Full reference matrix once, pairwise representatives for the other
        // DPI / locale / shell-style variants, while still checking every fixture.
        if(!fullMatrix && i!=0 && i!=4) continue;
        for(const auto &id:ids){
            if(!fullMatrix && id!=QStringLiteral("pastel-mint-cream") && id!=QStringLiteral("oled-black")) continue;
            CHECK(theme::applyLivePalette(app,id));
            for(const QSize size:{QSize(900,600),QSize(1280,800)}){
                tabs->setCurrentIndex(0);
                root->resize(size);root->update();app.processEvents();
                CHECK(root->size()==size);
                CHECK(picker->width()>=picker->minimumSizeHint().width());
                CHECK(apply->width()>=apply->minimumSizeHint().width());
                for (int tab=0;tab<tabs->count();++tab) {
                tabs->setCurrentIndex(tab);app.processEvents();
                const QString name=QStringLiteral("%1-%2-%3x%4-tab%5.png").arg(i).arg(id).arg(size.width()).arg(size.height()).arg(tab);
                const QImage before=root->grab().toImage(),after=root->grab().toImage();
                CHECK(!before.isNull() && before.save(QDir(output).filePath(name)));
                CHECK(difference(before,after)<=0.005);
                if(!baseline.isEmpty()){
                    const QImage expected(QDir(baseline).filePath(QStringLiteral("%1/%2").arg(variant,name)));
                    QImage diff;const double changed=difference(expected,before,&diff);
                    if(changed>0.005){
                        if(!expected.isNull()) expected.save(QDir(output).filePath(QStringLiteral("expected-")+name));
                        if(!diff.isNull()) diff.save(QDir(output).filePath(QStringLiteral("diff-")+name));
                        std::fprintf(stderr,"Baseline mismatch or missing: %s (%f)\n",qPrintable(name),changed);return 1;
                    }
                }
                }
            }
        }
    }
    QImage a(20,20,QImage::Format_RGB32),b(20,20,QImage::Format_RGB32);a.fill(Qt::black);b.fill(Qt::white);
    CHECK(difference(a,b)==1. && difference(a,a)==0.);
    return 0;
}
