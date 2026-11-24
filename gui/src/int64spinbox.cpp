#include "int64spinbox.h"
#include "integerunits.h"
#include <QRegularExpression>

#include <QLineEdit>
#include <limits>

Int64SpinBox::Int64SpinBox(QWidget *parent) : QAbstractSpinBox(parent) {
    // Typed text becomes the value on Enter / focus-out (or per keystroke with keyboard tracking).
    connect(this, &QAbstractSpinBox::editingFinished, this, &Int64SpinBox::commitText);
    connect(lineEdit(), &QLineEdit::textEdited, this, [this] { if (keyboardTracking()) commitText(); });
    showValue();
}

void Int64SpinBox::setRange(qint64 min, qint64 max) {
    if (max < min) max = min;
    min_ = min;
    max_ = max;
    setValue(value_);
    showValue();
}

void Int64SpinBox::setValue(qint64 v) {
    v = qBound(min_, v, max_);
    const bool changed = v != value_;
    value_ = v;
    showValue();
    if (changed) Q_EMIT valueChanged(value_);
}

bool Int64SpinBox::parse(const QString &text, qint64 *out) {
    bool ok = false;
    const qint64 n = text.trimmed().toLongLong(&ok);  // fails on overflow instead of wrapping
    if (ok && out) *out = n;
    return ok;
}

void Int64SpinBox::enableByteUnits() {
    byteUnits_ = true;
    timeUnit_ = integerunits::TimeUnit::None;
    // An opt-in editor must accept the full suffix before committing; otherwise
    // keyboard tracking would replace it with the canonical value mid-keystroke.
    setKeyboardTracking(false);
    lineEdit()->setToolTip(tr("Bytes. Enter an integer in B, KiB, MiB, GiB or TiB; stored as exact bytes."));
    setAccessibleDescription(lineEdit()->toolTip());
}

void Int64SpinBox::enableTimeUnits(integerunits::TimeUnit canonical) {
    const auto unit = integerunits::unitName(canonical);
    if (unit.empty()) return;
    byteUnits_ = false;
    timeUnit_ = canonical;
    setKeyboardTracking(false);
    lineEdit()->setToolTip(tr("Canonical unit: %1. Enter ns, us, ms or s (decimal point allowed only for exact conversions). Negative special values must be entered without a unit.")
        .arg(QString::fromLatin1(unit.data(), qsizetype(unit.size()))));
    setAccessibleDescription(lineEdit()->toolTip());
}

bool Int64SpinBox::parseEditor(const QString &text, qint64 *out) const {
    if (!hasExplicitUnits()) return parse(text, out);
    QString normalized = text;
    if (timeUnit_ != integerunits::TimeUnit::None) {
        normalized.replace(QChar(0x00b5), QLatin1Char('u'));
        normalized.replace(QChar(0x03bc), QLatin1Char('u'));
    }
    const auto utf8 = normalized.toUtf8();
    const std::string_view view(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    const auto result = byteUnits_ ? integerunits::bytes(view) : integerunits::time(view, timeUnit_);
    if (!result) return false;
    if (out) *out = *result;
    return true;
}

qint64 Int64SpinBox::stepped(qint64 v, qint64 step, int steps, qint64 min, qint64 max) {
    qint64 d = 0, r = 0;
    if (__builtin_mul_overflow(step, qint64(steps), &d) || __builtin_add_overflow(v, d, &r))
        return steps > 0 ? max : min;
    return qBound(min, r, max);
}

void Int64SpinBox::stepBy(int steps) {
    commitText();
    setValue(stepped(value_, 1, steps, min_, max_));
    selectAll();
}

QAbstractSpinBox::StepEnabled Int64SpinBox::stepEnabled() const {
    StepEnabled e = StepNone;
    if (value_ < max_) e |= StepUpEnabled;
    if (value_ > min_) e |= StepDownEnabled;
    return e;
}

QValidator::State Int64SpinBox::validate(QString &input, int &) const {
    const QString t = input.trimmed();
    if (t.isEmpty() || (t == QLatin1String("-") && min_ < 0)) return QValidator::Intermediate;
    if (timeUnit_ != integerunits::TimeUnit::None) {
        if (t.size() > 128) return QValidator::Invalid;
        qint64 n = 0;
        if (parseEditor(t, &n) && n >= min_ && n <= max_) return QValidator::Acceptable;
        // Allow incomplete numeric input even outside the canonical range: a
        // later suffix can make it valid (e.g. 1000 ns in a 10 us editor).
        static const QRegularExpression partial(QStringLiteral("^[0-9]+(?:\\.[0-9]*)?\\s*(?:[numµμ])?$"));
        if (partial.match(t).hasMatch()) return QValidator::Intermediate;
        return QValidator::Invalid;
    }
    if (byteUnits_) {
        qint64 n = 0;
        if (parseEditor(t, &n)) {
            if (n > max_) return QValidator::Invalid;
            return n >= min_ ? QValidator::Acceptable : QValidator::Intermediate;
        }
        static const QRegularExpression partial(QStringLiteral("^[0-9]+\\s*(?:[KMGT](?:i)?)$"));
        if (partial.match(t).hasMatch()) return QValidator::Intermediate;
        return QValidator::Invalid;
    }
    for (int i = 0; i < t.size(); ++i)
        if (!t[i].isDigit() && !(i == 0 && t[i] == QLatin1Char('-') && min_ < 0)) return QValidator::Invalid;
    qint64 n = 0;
    if (!parse(t, &n)) return QValidator::Invalid;  // beyond 64 bits
    if (n > max_ && n >= 0) return QValidator::Invalid;  // more digits cannot bring it back
    return n >= min_ && n <= max_ ? QValidator::Acceptable : QValidator::Intermediate;
}

void Int64SpinBox::fixup(QString &input) const {
    qint64 n = value_;
    if (parseEditor(input, &n)) {
        if (hasExplicitUnits() && (n < min_ || n > max_)) n = value_;
        else n = qBound(min_, n, max_);
    }
    input = QString::number(n);
}

void Int64SpinBox::commitText() {
    qint64 n = 0;
    if (parseEditor(lineEdit()->text(), &n) && (!hasExplicitUnits() || (n >= min_ && n <= max_))) setValue(n);
    else showValue();
}

void Int64SpinBox::showValue() {
    const QString t = QString::number(value_);
    if (lineEdit()->text() != t) lineEdit()->setText(t);
}
