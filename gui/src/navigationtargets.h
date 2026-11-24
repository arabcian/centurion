#pragma once
// Metadata only: registering or collecting targets never probes hardware or constructs lazy content.
#include <QCoreApplication>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTabWidget>
#include <QScrollArea>
#include <QVariant>
#include <QWidget>

namespace navigationtargets {
struct Entry { QString id, title, aliases; };
inline constexpr const char *Id = "centurionNavigationId";
inline void mark(QWidget *w, const QString &id, const QString &title, const QString &aliases = {}) {
    if (!w || id.isEmpty() || id.size() > 512) return;
    w->setProperty(Id, id);
    w->setProperty("centurionNavigationTitle", title);
    w->setProperty("centurionNavigationAliases", aliases);
}
// Removed tab contents can survive until deleteLater. Retire their IDs before
// the replacement editor is built, without changing QObject ownership.
inline void forget(QWidget *page) {
    if (!page) return;
    page->setProperty(Id, QVariant());
    for (QWidget *w : page->findChildren<QWidget *>())
        if (w->property(Id).isValid()) w->setProperty(Id, QVariant());
}
inline QList<Entry> entries(QWidget *page) {
    QList<Entry> out;
    QSet<QString> seen, ambiguous;
    if (!page) return out;
    for (QWidget *w : page->findChildren<QWidget *>()) {
        const QString id = w->property(Id).toString();
        if (id.isEmpty()) continue;
        if (seen.contains(id)) { ambiguous.insert(id); continue; }
        seen.insert(id);
        if (out.size() < 2048) out.append({id, w->property("centurionNavigationTitle").toString(), w->property("centurionNavigationAliases").toString()});
    }
    out.removeIf([&](const Entry &e) { return ambiguous.contains(e.id); });
    return out;
}
inline QWidget *resolve(QWidget *page, const QString &id) {
    if (!page || id.isEmpty()) return nullptr;
    QWidget *match = nullptr;
    for (QWidget *w : page->findChildren<QWidget *>()) {
        if (w->property(Id).toString() != id) continue;
        if (match) return nullptr; // ambiguity must never select the wrong device/row
        match = w;
    }
    return match;
}
// Call only after the user accepts a search result and the outer page is selected.
// Re-resolve after each nested tab switch: showEvent may replace the old editor.
inline bool reveal(QWidget *page, const QString &id) {
    for (int depth = 0; depth < 16; ++depth) {
        QPointer<QWidget> target = resolve(page, id);
        if (!target) return false;
        QPointer<QTabWidget> next;
        int index = -1;
        for (QWidget *p = target->parentWidget(); p && p != page; p = p->parentWidget()) {
            auto *tabs = qobject_cast<QTabWidget *>(p);
            if (!tabs) continue;
            for (int i = 0; i < tabs->count(); ++i) {
                QWidget *child = tabs->widget(i);
                if (child != target.data() && !child->isAncestorOf(target.data())) continue;
                if (!tabs->isTabEnabled(i) || !tabs->isTabVisible(i)) return false;
                if (tabs->currentIndex() != i) { next = tabs; index = i; }
                break;
            }
        }
        if (next) { next->setCurrentIndex(index); continue; }
        if (!target->isVisibleTo(page)) return false;
        for (QWidget *p = target->parentWidget(); p; p = p->parentWidget())
            if (auto *scroll = qobject_cast<QScrollArea *>(p)) { scroll->ensureWidgetVisible(target.data()); break; }
        if (target && target->isEnabled() && target->focusPolicy() != Qt::NoFocus) target->setFocus(Qt::OtherFocusReason);
        return true;
    }
    return false;
}
// AMD overdrive controls are created only after a capability response. These
// names are searchable without requesting one; missing controls stay unavailable.
struct AmdEntry { const char *id, *title, *aliases; };
inline constexpr AmdEntry amdEntries[] = {
    {"amd.perf", "Performance level", "DPM performans seviyesi"},
    {"amd.profile", "Power profile", "SMU power güç profili"},
    {"amd.power", "Power limit", "TDP watt güç limiti"},
    {"amd.sclk.states", "GPU states", "DPM GPU çekirdek durumları"},
    {"amd.mclk.states", "Memory states", "VRAM DPM bellek durumları"},
    {"amd.sclk.min", "GPU clock minimum", "SCLK minimum çekirdek frekansı"},
    {"amd.sclk.max", "GPU clock maximum", "SCLK maksimum çekirdek frekansı"},
    {"amd.sclk.offset", "GPU clock offset", "SCLK çekirdek frekans ofseti"},
    {"amd.mclk.min", "Memory clock minimum", "MCLK VRAM minimum bellek frekansı"},
    {"amd.mclk.max", "Memory clock maximum", "MCLK VRAM maksimum bellek frekansı"},
    {"amd.vf", "V/F curve", "voltage frequency voltaj frekans eğrisi"},
    {"amd.voltage", "Voltage offset", "undervolt voltaj ofseti"},
    {"amd.zero_rpm", "Zero RPM", "fan stop sıfır devir"},
    {"amd.fan_curve", "Fan curve", "fan eğrisi hotspot sıcaklık"},
    {"amd.fan.zero_rpm_stop", "Zero-RPM stop below (°C)", "fan stop sıcaklık"},
    {"amd.fan.min_pwm", "Minimum fan speed (%)", "minimum fan hızı PWM"},
    {"amd.fan.target_temp", "Target temperature (°C)", "hedef sıcaklık"},
    {"amd.fan.acoustic_limit", "Acoustic limit (RPM)", "akustik fan sınırı"},
    {"amd.fan.acoustic_target", "Acoustic target (RPM)", "akustik fan hedefi"},
};
inline void markAmd(QWidget *w, const QString &name) {
    for (const auto &e : amdEntries)
        if (name == QLatin1String(e.title)) { mark(w, QString::fromLatin1(e.id), name, QString::fromUtf8(e.aliases)); return; }
}
} // namespace navigationtargets
