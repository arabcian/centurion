#include "fanassist.h"
#include "fwattrtab.h"
#include "platformprofile.h"
#include "privileged.h"
#include "theme.h"
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

// Without cpu_temp the CPU throttles at its own TjMax, which the curve then assumes to be this.
static constexpr int TJMAX_C = 100;

// The stock presets: fan-presets.json from the repository root, compiled in. Written to the user's presets file
// when it does not exist yet, and used when that file cannot be read.
static const QString STOCK = QStringLiteral(":/fan-presets.json");

static QVector<int> legal(QVector<int> lv);

static QString presetsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/centurion/fan-presets.json");
}

/// The presets in `data`; the error text (and no presets) when it is invalid.
static QVector<FanPreset> parsePresets(const QByteArray &data, QString *error) {
    auto bad = [&](const QString &why) { *error = why; return QVector<FanPreset>(); };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) return bad(pe.errorString() + QStringLiteral(" at byte %1").arg(pe.offset));
    QVector<FanPreset> out;
    for (const QJsonValue &v : doc.object().value("presets").toArray()) {
        const QJsonObject o = v.toObject();
        FanPreset p{o.value("name").toString(), o.value("level").toInt(-1), o.value("cpu_min").toInt(-1),
                    o.value("cpu_max").toInt(-1), o.value("ramp_from").toInt(-1), {}};
        const QString n = QStringLiteral("preset %1").arg(out.size() + 1);
        if (p.name.isEmpty()) return bad(n + QStringLiteral(": \"name\" is missing"));
        if (p.cpuMin < 1 || p.cpuMax < p.cpuMin) return bad(n + QStringLiteral(": needs 0 < \"cpu_min\" ≤ \"cpu_max\""));
        if (o.contains("curve")) {
            // Taken as it is: one level per temperature step, as in the editor.
            for (const QJsonValue &x : o.value("curve").toArray()) p.curve << x.toInt(-1);
            if (p.curve.size() != 10 || std::any_of(p.curve.cbegin(), p.curve.cend(), [](int l) { return l < 1 || l > 10; }))
                return bad(n + QStringLiteral(": \"curve\" must be 10 levels, each 1–10"));
            if (legal(p.curve) != p.curve)
                return bad(n + QStringLiteral(": \"curve\" must never go down, and its last three steps must be at least 3 / 5 / 7"));
        } else {
            if (p.level < 1 || p.level > 10) return bad(n + QStringLiteral(": \"level\" must be 1–10"));
            if (p.rampFrom < 0 || p.rampFrom > 9) return bad(n + QStringLiteral(": \"ramp_from\" is a temperature step, 0–9"));
        }
        out << p;
    }
    if (out.isEmpty()) return bad(QStringLiteral("no \"presets\" array"));
    return out;
}

/// The presets file's presets; the stock ones (and `error` set) when it is missing — then it is written — or invalid.
static QVector<FanPreset> loadPresets(QString *error) {
    QFile stock(STOCK);
    const QByteArray stockData = stock.open(QIODevice::ReadOnly) ? stock.readAll() : QByteArray();
    QString ignored;
    const QVector<FanPreset> defaults = parsePresets(stockData, &ignored);
    QFile f(presetsFile());
    if (!f.exists()) {
        QDir().mkpath(QFileInfo(f).absolutePath());
        QSaveFile out(presetsFile());
        if (out.open(QIODevice::WriteOnly)) { out.write(stockData); out.commit(); }
        return defaults;
    }
    if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024) { *error = QStringLiteral("cannot be read"); return defaults; }
    const QVector<FanPreset> out = parsePresets(f.readAll(), error);
    return error->isEmpty() ? out : defaults;
}

/// Non-decreasing, 1..10, three hottest steps at least 3 / 5 / 7 (legion-profile-helper's thermal floor).
static QVector<int> legal(QVector<int> lv) {
    for (int &x : lv) x = std::clamp(x, 1, 10);
    for (int i = 1; i < lv.size(); ++i) lv[i] = std::max(lv[i], lv[i - 1]);
    if (lv.size() == 10) { lv[7] = std::max(lv[7], 3); lv[8] = std::max(lv[8], 5); lv[9] = std::max(lv[9], 7); }
    return lv;
}

QVector<int> FanAssistDialog::shape(double limit, int hold, int rampFrom, const QVector<int> &temps) {
    hold = std::clamp(hold, 1, 10);
    auto at = [&](int s) { return double(temps.value(s, 40 + 6 * s)); };  // 127 = never reached
    // The step the EC is on while the CPU sits at its limit: the last threshold the limit reaches.
    int a = 0;
    for (int s = 0; s < 10; ++s) if (at(s) <= limit) a = s;
    const int start = std::clamp(rampFrom, 0, 9);
    QVector<int> lv(10);
    for (int s = 0; s < 10; ++s) {
        int l;
        if (s == a) l = hold;
        else if (s < a) l = s <= start ? 1 : int(std::lround(1 + double(hold - 1) * (s - start) / (a - start)));
        else l = at(s) < limit + 8 ? hold : 10;  // above the limit: only if the CPU overshoots it
        lv[s] = l;
    }
    return legal(lv);
}

/// The CPU fan: the one whose step thresholds reach the hottest (127 = never). Sensor ids differ per model —
/// on one Legion sensor 1's steps end at 47 °C — and only the CPU's run up near its throttle point.
const FanCurveFan *FanAssistDialog::cpuFan() const {
    const FanCurveFan *f = nullptr;
    int best = -1;
    for (const FanCurveFan &x : fans_) {
        if (x.temp.size() != 10) continue;
        int hi = 0;
        for (int t : x.temp) if (t < 127) hi = std::max(hi, t);
        if (hi > best) { best = hi; f = &x; }
    }
    return f;
}

FanAssistDialog::FanAssistDialog(const QVector<FanCurveFan> &fans, QWidget *parent) : QDialog(parent), fans_(fans) {
    setWindowTitle(QStringLiteral("Fan curve assistant"));
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "Sets the firmware's CPU and GPU temperature limits — the chip throttles there instead of running hotter — and "
        "proposes a curve for a noise level. Under sustained load the CPU sits at its limit, so the noise level is the fan "
        "level the curve holds there. The limits are set when you put the proposal in the editor (Custom profile); press "
        "Apply there to keep the curve."));
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *form = new QFormLayout;
    for (auto [lim, label] : {std::pair{&cpu_, "CPU limit"}, std::pair{&gpu_, "GPU limit"}}) {
        lim->spin = new QSpinBox;
        lim->spin->setSuffix(QStringLiteral(" °C"));
        lim->spin->setRange(0, 0);
        lim->spin->setSpecialValueText(QStringLiteral("not exposed by the firmware"));
        lim->spin->setEnabled(false);
        form->addRow(QString::fromLatin1(label), lim->spin);
    }
    for (const FwAttr &a : FwattrTab::discover()) {
        Limit *l = a.name == QLatin1String("cpu_temp") ? &cpu_ : a.name == QLatin1String("gpu_temp") ? &gpu_ : nullptr;
        if (!l || !a.ranged || a.viaWmi() || a.readOnly) continue;
        l->path = a.path;
        l->current = a.current;
        l->min = a.min;
        l->max = a.max;
        l->step = a.step;
        l->spin->setSpecialValueText(QString());
        l->spin->setRange(a.min, a.max);
        l->spin->setSingleStep(a.step);
        l->spin->setValue(a.current);
        l->spin->setEnabled(true);
    }
    QString error;
    presets_ = loadPresets(&error);
    noise_ = new QComboBox;
    for (const FanPreset &p : presets_)
        noise_->addItem((p.curve.isEmpty() ? QStringLiteral("%1 — fan level %2 at the CPU limit").arg(p.name).arg(p.level)
                                           : QStringLiteral("%1 — own curve").arg(p.name))
                        + QStringLiteral(", CPU %1–%2 °C").arg(p.cpuMin).arg(p.cpuMax));
    noise_->setCurrentIndex(std::min<int>(1, presets_.size() - 1));
    applyPreset();
    form->addRow(QStringLiteral("Fan noise"), noise_);
    v->addLayout(form);
    auto *file = new QLabel(error.isEmpty()
        ? QStringLiteral("Presets: %1 — edit it and reopen the assistant.").arg(presetsFile().toHtmlEscaped())
        : QStringLiteral("<span style='color:%1'>%2: %3 — using the built-in presets.</span>")
              .arg(theme::WARN, presetsFile().toHtmlEscaped(), error.toHtmlEscaped()));
    file->setWordWrap(true);
    file->setTextFormat(Qt::RichText);
    file->setProperty("role", "muted");
    v->addWidget(file);

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::RichText);
    v->addWidget(status_);
    auto *b = new QHBoxLayout;
    b->addStretch(1);
    use_ = new QPushButton(QStringLiteral("Set limits and put the curve in the editor"));
    use_->setObjectName("btnAccent");
    auto *close = new QPushButton(QStringLiteral("Cancel"));
    b->addWidget(use_);
    b->addWidget(close);
    v->addLayout(b);
    connect(close, &QPushButton::clicked, this, &FanAssistDialog::reject);
    connect(use_, &QPushButton::clicked, this, &FanAssistDialog::apply);
    connect(cpu_.spin, &QSpinBox::valueChanged, this, &FanAssistDialog::propose);
    connect(noise_, &QComboBox::currentIndexChanged, this, [this] { applyPreset(); propose(); });
    propose();
}

void FanAssistDialog::applyPreset() {
    if (cpu_.path.isEmpty()) return;
    // The preset's range, kept inside the firmware's; QSpinBox pulls the value into it.
    const FanPreset &p = presets_[noise_->currentIndex()];
    const int lo = std::clamp(p.cpuMin, cpu_.min, cpu_.max), hi = std::clamp(p.cpuMax, lo, cpu_.max);
    cpu_.spin->setRange(lo, hi);
}

void FanAssistDialog::propose() {
    const FanCurveFan *f = cpuFan();
    const FanPreset &p = presets_[noise_->currentIndex()];
    result_ = !p.curve.isEmpty() ? p.curve
                                 : shape(cpu_.path.isEmpty() ? TJMAX_C : cpu_.spin->value(), p.level, p.rampFrom, f ? f->temp : QVector<int>());
    QStringList l;
    for (int x : result_) l << QString::number(x);
    status_->setText(QStringLiteral("Proposed curve: <b>%1</b>%2").arg(l.join(QStringLiteral(" · ")),
                     !p.curve.isEmpty() ? QStringLiteral(" (the preset's own curve)")
                     : f ? QStringLiteral(" (on the steps of fan %1, sensor %2)").arg(f->id).arg(f->sensor)
                         : QStringLiteral(" (no per-fan temperature steps from the EC: generic spacing)")));
}

void FanAssistDialog::apply() {
    QJsonArray items;
    for (Limit *l : {&cpu_, &gpu_}) {
        if (l->path.isEmpty()) continue;
        const int v = l->min + (l->spin->value() - l->min) / l->step * l->step;  // onto the firmware's grid
        if (v != l->current) items.append(QJsonObject{{"path", l->path}, {"value", v}});
    }
    if (items.isEmpty()) { accept(); return; }
    if (pp::currentProfile(pp::primaryHandler()) != QStringLiteral("custom")) {
        status_->setText(QStringLiteral("<span style='color:%1'>Switch to the Custom power profile first: the firmware takes these limits only there.</span>").arg(theme::WARN));
        return;
    }
    use_->setEnabled(false);
    privileged::run(privileged::helperPath(QStringLiteral("fwattr-helper")), QJsonDocument(items).toJson(QJsonDocument::Compact), this,
                    [this](const privileged::Result &r) {
        if (r.ok()) { accept(); return; }
        // A batch reports per item: {"ok": false, "results": [{"path", "ok", "error"}, ...]}.
        QStringList why;
        for (const QJsonValue &x : r.json.value("results").toArray())
            if (!x.toObject().value("ok").toBool()) why << x.toObject().value("error").toString();
        if (why.isEmpty()) why << r.message();
        use_->setEnabled(true);
        status_->setText(QStringLiteral("<span style='color:%1'>Could not set the limits: %2</span>").arg(theme::DANGER, why.join(QStringLiteral("; ")).toHtmlEscaped()));
    });
}
