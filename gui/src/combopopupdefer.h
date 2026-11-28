#pragma once
// Application event filter: a combo box that has never been shown gets its popup only when it is.
//
// QComboBox builds its popup (a hidden top-level frame, a list view, two scroll bars, two scrollers:
// nine widgets) on every StyleChange and FontChange, and under the application style sheet every
// combo gets both while the window is built, so each costs ~100 KiB more for as long as the program
// runs, whether or not its page is ever opened. Until a combo is first shown (and while it has no
// popup), those two events are held back; they are delivered, in order, when the combo is shown, so
// it is restyled and re-measured exactly as it would have been, before anything of it is on screen.
// QComboBox itself re-measures on its first show (AdjustToContentsOnFirstShow), so its geometry is
// settled at that point in any case.
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>

class ComboPopupDefer final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *e) override {
        const QEvent::Type type = e->type();
        if (type != QEvent::StyleChange && type != QEvent::FontChange && type != QEvent::Show) return false;
        auto *combo = qobject_cast<QComboBox *>(obj);
        if (!combo || combo->property(SHOWN).toBool()) return false;
        if (type == QEvent::Show) {
            combo->setProperty(SHOWN, true);  // from now on every event goes straight through
            const int pending = combo->property(PENDING).toInt();
            combo->setProperty(PENDING, QVariant());
            if (pending & STYLE) { QEvent style(QEvent::StyleChange); QCoreApplication::sendEvent(combo, &style); }
            if (pending & FONT) { QEvent font(QEvent::FontChange); QCoreApplication::sendEvent(combo, &font); }
            return false;
        }
        // A combo whose popup already exists (code asked for its view) keeps Qt's own handling.
        for (const QObject *child : combo->children())
            if (child->inherits("QComboBoxPrivateContainer")) return false;
        combo->setProperty(PENDING, combo->property(PENDING).toInt() | (type == QEvent::StyleChange ? STYLE : FONT));
        return true;
    }

private:
    static constexpr const char *SHOWN = "_centurion_comboShown";
    static constexpr const char *PENDING = "_centurion_comboPending";
    enum : int { STYLE = 1, FONT = 2 };
};
