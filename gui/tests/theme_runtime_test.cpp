// Isolated offscreen presentation and settings; no hardware access or real user config writes.
#include "../src/theme.h"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QByteArray>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "Failed: %s\n", #x); return 1; } } while (false)
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
    qunsetenv("CENTURION_UI");
    CHECK(theme::saveUiMode(true));
    theme::load();
    CHECK(theme::experimentalEnabled());
    CHECK(theme::saveUiMode(false));
    CHECK(theme::experimentalEnabled()); // pending choice isn't the running shell
    theme::load();
    CHECK(!theme::experimentalEnabled());
    qputenv("CENTURION_UI", QByteArray("experimental"));
    theme::load();
    CHECK(theme::experimentalEnabled());
    theme::setRestartGuard([] { return false; });
    theme::requestRestart();
    CHECK(!theme::restartRequested());
    theme::setRestartGuard({});
    CHECK(theme::saveCompactDensity(true));
    CHECK(!theme::compactDensity());
    theme::load(); CHECK(theme::compactDensity());
    CHECK(theme::saveCompactDensity(false)); theme::load(); CHECK(!theme::compactDensity());
    CHECK(theme::typographyPreference().points == theme::FONT_PT);
    CHECK(!theme::saveTypography({{}, {}, 100}));
    CHECK(!theme::saveTypography({QStringLiteral("bad;family"), {}, 9}));
    CHECK(theme::saveTypography({QStringLiteral("Missing font"), QStringLiteral("Missing fixed font"), 12}));
    CHECK(theme::typographyPreference().points == 12);
    CHECK(theme::typographyPreference().numericFamily == QStringLiteral("Missing fixed font"));
    CHECK(theme::saveTypography({}));
    CHECK(theme::saveTheme(QStringLiteral("oled-black"))); theme::load();
    CHECK(QByteArray(theme::BG0) == QByteArray("#000000"));
    const auto original = theme::paletteColors(QStringLiteral("graphite"));
    CHECK(original.size() == 19);
    auto custom = original;
    custom[8] = QStringLiteral("#aabbcc");
    CHECK(!theme::saveCustomPalette({QStringLiteral("#ffffff")}));
    CHECK(theme::saveCustomPalette(custom));
    auto malformed = custom;
    malformed[8] += QLatin1Char('\n');
    CHECK(!theme::validCustomPalette(malformed));
    CHECK(!theme::saveCustomPalette(malformed));
    CHECK(theme::paletteColors(QStringLiteral("custom")) == custom);
    malformed[8] = QStringLiteral("#aabbc");
    CHECK(!theme::saveCustomPalette(malformed));
    malformed[8] = QStringLiteral("#aabbcg");
    CHECK(!theme::saveCustomPalette(malformed));
    CHECK(theme::saveTheme(QStringLiteral("custom")));
    theme::load();
    CHECK(theme::currentTheme() == QStringLiteral("custom"));
    CHECK(QByteArray(theme::FG) == QByteArray("#aabbcc"));
    CHECK(theme::paletteColors(QStringLiteral("custom")) == custom);
    CHECK(theme::saveTheme(QStringLiteral("graphite")));
    theme::load();
    CHECK(theme::paletteColors(theme::currentTheme()) == original);
    const char *const address = theme::FG;
    for (int i=0; i<20; ++i) {
        CHECK(theme::saveTheme(i%2 ? QStringLiteral("graphite") : QStringLiteral("custom")));
        theme::load(); CHECK(theme::FG == address);
        CHECK(QByteArray(theme::FG) == (i%2 ? original[8] : custom[8]).toLatin1());
    }
    CHECK(theme::contrastRatio(QColor(Qt::black), QColor(Qt::white)) > 20.99);
    CHECK(theme::contrastRatio(QColor(Qt::black), QColor(Qt::black)) == 1.0);
    CHECK(theme::contrastChecks({}).isEmpty());
    for (const auto &entry : theme::themes()) {
        const auto checks = theme::contrastChecks(theme::paletteColors(entry.first));
        CHECK(!checks.isEmpty());
        bool selection = false, disabled = false;
        for (const auto &check : checks) {
            CHECK(check.ratio >= 1 && check.ratio <= 21.00001);
            if (check.role == QStringLiteral("Selection text")) { CHECK(check.ratio >= 4.5); selection = true; }
            if (check.exempt) disabled = true;
        }
        CHECK(selection && disabled);
    }
    auto lowContrast=original;
    for (int i : {8,9,10,11,12,13,14,15,17,18}) lowContrast[i]=lowContrast[2];
    const auto repaired=theme::repairPaletteContrast(lowContrast);
    CHECK(repaired.has_value());
    for (int i=0;i<8;++i) CHECK((*repaired)[i]==lowContrast[i]);
    CHECK((*repaired)[16]==lowContrast[16]);
    for (const auto &c:theme::contrastChecks(*repaired)) CHECK(c.exempt || c.ratio>=c.target);
    CHECK(theme::repairPaletteContrast(*repaired)==repaired);
    CHECK(!theme::repairPaletteContrast({}));
    auto impossible=lowContrast;
    impossible[0]=QStringLiteral("#ffffff"); impossible[1]=QStringLiteral("#000000"); impossible[2]=QStringLiteral("#808080");
    CHECK(!theme::repairPaletteContrast(impossible));
    for (const auto &entry:theme::themes()) {
        if (entry.first==QStringLiteral("custom")) continue;
        for(const auto &c:theme::contrastChecks(theme::paletteColors(entry.first))) CHECK(c.exempt || c.ratio>=c.target);
    }
    theme::apply(app);
    QLabel label;
    QLineEdit draft;
    draft.setText(QStringLiteral("unchanged")); draft.setSelection(1,3);
    int calls=0; int nestedRejected=0;
    theme::bindPresentation(&label,[&] {
        ++calls; theme::setSheet(&label,QStringLiteral("color:%1").arg(theme::WARN));
        if(calls>1 && !theme::applyLivePalette(app,QStringLiteral("graphite"))) ++nestedRejected;
    });
    auto settingsBytes=[&] {
        QSettings s(QSettings::IniFormat,QSettings::UserScope,QStringLiteral("centurion"),QStringLiteral("gui"));
        QFile f(s.fileName()); return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};
    };
    const QByteArray before=settingsBytes(); CHECK(!before.isEmpty());
    const quint64 initial=theme::revision();
    for (int i=0;i<20;++i) {
        const QString id=i%2?QStringLiteral("graphite"):QStringLiteral("pastel-mint-cream");
        CHECK(theme::applyLivePalette(app,id)); app.processEvents();
        CHECK(theme::profileAccent(QStringLiteral("performance"))==QString::fromLatin1(theme::DANGER));
        CHECK(theme::profileAccent(QStringLiteral("quiet"))==QString::fromLatin1(theme::INFO));
        CHECK(label.styleSheet()==QStringLiteral("color:%1").arg(theme::WARN));
        CHECK(draft.text()==QStringLiteral("unchanged") && draft.selectionStart()==1 && draft.selectedText()==QStringLiteral("nch"));
        CHECK(theme::revision()==initial+quint64(i)+1 && theme::lastLiveRefreshNanos()>=0);
    }
    CHECK(calls==21 && nestedRejected==20 && before==settingsBytes());
    int deletedCalls=0;
    auto *deleted=new QLabel;
    theme::bindPresentation(deleted,[&]{++deletedCalls;}); delete deleted;
    CHECK(theme::applyLivePalette(app,QStringLiteral("oled-black")) && deletedCalls==1);
    CHECK(!theme::applyLivePalette(app,QStringLiteral("not-a-theme")) && before==settingsBytes());
    CHECK(!theme::restartRequested());
    // Normal-shell preference APIs use the same snapshot as the visible palette.
    QWidget bound;
    int oldBinding=0, newBinding=0;
    theme::setLiveSheet(&bound,[&]{++oldBinding; return QStringLiteral("color:%1").arg(theme::WARN);});
    theme::setLiveSheet(&bound,[&]{++newBinding; return QStringLiteral("color:%1").arg(theme::OK);});
    const auto children=bound.children().size();
    draft.setText(QStringLiteral("draft")); draft.end(false); draft.insert(QStringLiteral(" edit"));
    CHECK(draft.isUndoAvailable());
    for(int i=0;i<20;++i) {
        const auto id=i%2?QStringLiteral("graphite"):QStringLiteral("pastel-mint-cream");
        CHECK(theme::applyLivePreference(app,id));
        CHECK(theme::currentTheme()==id);
        QSettings s(QSettings::IniFormat,QSettings::UserScope,QStringLiteral("centurion"),QStringLiteral("gui"));
        CHECK(s.value(QStringLiteral("theme")).toString()==id);
        CHECK(bound.styleSheet()==QStringLiteral("color:%1").arg(theme::OK));
        CHECK(bound.children().size()==children && draft.text()==QStringLiteral("draft edit"));
    }
    CHECK(oldBinding==1 && newBinding==21);
    draft.undo(); CHECK(draft.text()==QStringLiteral("draft"));
    CHECK(theme::applyLiveCustomPreference(app,custom));
    CHECK(theme::currentTheme()==QStringLiteral("custom"));
    CHECK(theme::paletteColors(QStringLiteral("custom"))==custom);
    const auto committed=settingsBytes(); const auto committedRevision=theme::revision();
    CHECK(!theme::applyLiveCustomPreference(app,malformed));
    CHECK(!theme::applyLivePreference(app,QStringLiteral("not-a-theme")));
    CHECK(settingsBytes()==committed && theme::revision()==committedRevision);
    CHECK(theme::currentTheme()==QStringLiteral("custom") && QByteArray(theme::FG)==custom[8].toLatin1());
    CHECK(!theme::restartRequested());
    return 0;
}
