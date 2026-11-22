#include "pipewireconfig.h"

#include "sysadmin.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Section { Core, Pulse, Wp, Alsa };
enum class Kind { Bool, Int, Float, Choice, List, Frac };

struct Opt {
    Section section;
    const char *key;
    Kind kind;
    const char *def;    // the built-in default, as shown ("" = no fixed default)
    const char *extra;  // Int / Float: "min:max"; Choice: "a|b|c"
    bool quoted;        // a Choice written as a string
    const char *name;
    const char *hint;
};

// What PipeWire and WirePlumber document for these options (pipewire.conf, wireplumber.conf's settings schema).
const Opt OPTS[] = {
    {Core, "default.clock.rate", Kind::Choice, "48000", "44100|48000|88200|96000|192000", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Sample rate"),
     QT_TRANSLATE_NOOP("PipewireConfig", "The rate the sound graph runs at when no stream asks for another. 48000 suits most hardware and video; 44100 only matters for CD-style music.")},
    {Core, "default.clock.allowed-rates", Kind::List, "48000", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Allowed sample rates"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Rates PipeWire may switch to so a stream is played without resampling, for example 44100 48000 96000. Separate them with spaces.")},
    {Core, "default.clock.quantum", Kind::Int, "1024", "16:8192", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Buffer size (quantum)"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Buffer size in samples: at 48000 Hz, 1024 is about 21 ms. Lower means less delay but a higher risk of crackling (xruns).")},
    {Core, "default.clock.min-quantum", Kind::Int, "32", "16:8192", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Smallest buffer"),
     QT_TRANSLATE_NOOP("PipewireConfig", "The smallest buffer any program may ask for. Raise it (256 or 512) when a low-latency program makes everything crackle.")},
    {Core, "default.clock.max-quantum", Kind::Int, "2048", "16:8192", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Largest buffer"),
     QT_TRANSLATE_NOOP("PipewireConfig", "The largest buffer used while no program asks for low latency. A bigger value saves power and CPU time.")},
    {Core, "clock.power-of-two-quantum", Kind::Bool, "true", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Power-of-two buffers"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Round requested buffer sizes to a power of two. Turn it off only for a program that insists on other sizes.")},
    {Core, "log.level", Kind::Choice, "2", "0|1|2|3|4|5", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Log level"),
     QT_TRANSLATE_NOOP("PipewireConfig", "0 off, 1 errors, 2 warnings, 3 info, 4 debug, 5 trace. Raise it to 3 or 4 only while hunting a problem; the log is long.")},
    {Pulse, "pulse.min.req", Kind::Frac, "", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Pulse clients: smallest request"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Smallest request size for PulseAudio programs, written samples/rate, for example 256/48000 (5 ms). Raise it if a browser or game crackles.")},
    {Pulse, "pulse.min.quantum", Kind::Frac, "", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Pulse clients: smallest buffer"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Smallest buffer the Pulse server asks the graph for, written samples/rate, for example 512/48000.")},
    {Wp, "bluetooth.autoswitch-to-headset-profile", Kind::Bool, "true", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Bluetooth: switch to headset mode"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Switch a Bluetooth headset to its call profile (with microphone, lower sound quality) when a program records. Turn it off to keep the music profile.")},
    {Wp, "bluetooth.profile-preference", Kind::Choice, "quality", "quality|latency", true,
     QT_TRANSLATE_NOOP("PipewireConfig", "Bluetooth: preferred profile"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Prefer better quality or lower latency when a Bluetooth profile is chosen automatically.")},
    {Wp, "device.routes.default-sink-volume", Kind::Float, "0.064", "0:1", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Volume of a new output"),
     QT_TRANSLATE_NOOP("PipewireConfig", "The volume (0 to 1) a sound output gets the first time it is seen. 0.064 is about 40 % on the usual cubic volume scale.")},
    {Wp, "device.routes.mute-on-alsa-playback-removed", Kind::Bool, "false", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Mute when wired headphones are unplugged"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Mute all outputs when the active wired headphones or speakers disconnect, so sound does not suddenly come from the speakers.")},
    {Wp, "device.routes.mute-on-bluetooth-playback-removed", Kind::Bool, "false", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Mute when Bluetooth audio disconnects"),
     QT_TRANSLATE_NOOP("PipewireConfig", "The same for Bluetooth headphones and speakers.")},
    {Wp, "linking.pause-playback", Kind::Bool, "true", "", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Pause players when their output disappears"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Pause media players whose output device was removed.")},
    {Alsa, "session.suspend-timeout-seconds", Kind::Int, "5", "0:3600", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Idle suspend delay (s)"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Seconds an idle sound device waits before it is suspended. 0 keeps it open: it ends the pop or the cut-off first second some speakers and DACs give after silence, and costs a little power.")},
    {Alsa, "api.alsa.headroom", Kind::Int, "0", "0:65536", false,
     QT_TRANSLATE_NOOP("PipewireConfig", "Device buffer headroom (samples)"),
     QT_TRANSLATE_NOOP("PipewireConfig", "Extra samples of safety margin in the sound device's buffer. It can reduce crackling on some hardware, and adds that much delay.")},
};
constexpr int NOPTS = int(sizeof(OPTS) / sizeof(OPTS[0]));

QString tx(const char *s) { return QCoreApplication::translate("PipewireConfig", s); }

QString pathOf(Section s) {
    static const char *const DIRS[] = {"pipewire/pipewire.conf.d", "pipewire/pipewire-pulse.conf.d", "wireplumber/wireplumber.conf.d", "wireplumber/wireplumber.conf.d"};
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1Char('/') + QLatin1String(DIRS[s]) + QStringLiteral("/50-centurion.conf");
}

/// One block of `key = value` lines as the section's config syntax.
QString block(Section s, const QStringList &lines) {
    if (lines.isEmpty()) return {};
    QString out;
    if (s == Alsa) {
        out = QStringLiteral("monitor.alsa.rules = [\n  {\n    matches = [\n      { node.name = \"~alsa_output.*\" }\n      { node.name = \"~alsa_input.*\" }\n    ]\n    actions = {\n      update-props = {\n");
        for (const QString &l : lines) out += QStringLiteral("        ") + l + QLatin1Char('\n');
        return out + QStringLiteral("      }\n    }\n  }\n]\n");
    }
    static const char *const OPEN[] = {"context.properties = {", "pulse.properties = {", "wireplumber.settings = {"};
    out = QLatin1String(OPEN[s]) + QLatin1Char('\n');
    for (const QString &l : lines) out += QStringLiteral("    ") + l + QLatin1Char('\n');
    return out + QStringLiteral("}\n");
}

/// The value of `key` in text written by this dialog, without quotes or brackets; null when absent.
QString readValue(const QString &text, const Opt &o) {
    const auto m = QRegularExpression(QStringLiteral("^\\s*") + QRegularExpression::escape(QLatin1String(o.key)) + QStringLiteral("\\s*=\\s*(.+?)\\s*$"),
                                      QRegularExpression::MultilineOption).match(text);
    if (!m.hasMatch()) return {};
    QString v = m.captured(1);
    if (v.startsWith(QLatin1Char('[')) && v.endsWith(QLatin1Char(']'))) v = v.mid(1, v.size() - 2).trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) v = v.mid(1, v.size() - 2);
    return v;
}

QWidget *editor(const Opt &o, const QString &value) {
    const QString v = value.isNull() ? QLatin1String(o.def) : value;
    const QStringList range = QString::fromLatin1(o.extra).split(QLatin1Char(':'));
    switch (o.kind) {
    case Kind::Bool: { auto *w = new QCheckBox; w->setChecked(v == QLatin1String("true")); return w; }
    case Kind::Int: {
        auto *w = new QSpinBox;
        w->setRange(range.value(0).toInt(), range.value(1).toInt());
        w->setValue(v.toInt());
        return w;
    }
    case Kind::Float: {
        auto *w = new QDoubleSpinBox;
        w->setDecimals(3);
        w->setSingleStep(0.01);
        w->setRange(range.value(0).toDouble(), range.value(1).toDouble());
        w->setValue(v.toDouble());
        return w;
    }
    case Kind::Choice: {
        auto *w = new QComboBox;
        w->addItems(QString::fromLatin1(o.extra).split(QLatin1Char('|')));
        w->setCurrentText(v);
        return w;
    }
    default: return new QLineEdit(v);
    }
}

/// The value as written to the file; null when a text value is not valid.
QString valueOf(const Opt &o, const QWidget *w) {
    switch (o.kind) {
    case Kind::Bool: return static_cast<const QCheckBox *>(w)->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
    case Kind::Int: return QString::number(static_cast<const QSpinBox *>(w)->value());
    case Kind::Float: return QString::number(static_cast<const QDoubleSpinBox *>(w)->value(), 'g', 6);
    case Kind::Choice: {
        const QString t = static_cast<const QComboBox *>(w)->currentText();
        return o.quoted ? QLatin1Char('"') + t + QLatin1Char('"') : t;
    }
    case Kind::List: {
        const QStringList parts = static_cast<const QLineEdit *>(w)->text().split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
        static const QRegularExpression num(QStringLiteral("^[1-9]\\d{3,6}$"));
        if (parts.isEmpty()) return {};
        for (const QString &p : parts) if (!num.match(p).hasMatch()) return {};
        return QStringLiteral("[ ") + parts.join(QLatin1Char(' ')) + QStringLiteral(" ]");
    }
    case Kind::Frac: {
        const QString t = static_cast<const QLineEdit *>(w)->text().trimmed();
        static const QRegularExpression frac(QStringLiteral("^\\d{1,5}/\\d{4,6}$"));
        return frac.match(t).hasMatch() ? t : QString();
    }
    }
    return {};
}

/// "" on success. An empty `text` removes the file.
QString writeFile(const QString &path, const QString &text) {
    if (text.isEmpty()) {
        QFile f(path);
        return !f.exists() || f.remove() ? QString() : f.errorString();
    }
    if (!QDir().mkpath(QFileInfo(path).path())) return tx("The folder %1 cannot be created.").arg(QFileInfo(path).path());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return f.errorString();
    f.write(text.toUtf8());
    return f.commit() ? QString() : f.errorString();
}

const char *const HEADER = "# Written by Centurion (PipeWire Doctor → Settings). Delete this file to return these options to their defaults.\n";

}  // namespace

PipewireConfigDialog::PipewireConfigDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("PipeWire settings"));
    resize(900, 560);
    auto *v = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Tick an option to set it; unticked options keep PipeWire's own default. The choices are written to "
                                "50-centurion.conf files in ~/.config/pipewire and ~/.config/wireplumber, which override the system "
                                "files without changing them. Applying restarts the sound servers, so audio stops for a moment."));
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");
    v->addWidget(intro);

    table_ = new QTableWidget(NOPTS, 5);
    table_->setHorizontalHeaderLabels({tr("Set"), tr("Option"), tr("Value"), tr("Default"), tr("Setting")});
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    v->addWidget(table_, 1);

    hint_ = new QLabel;
    hint_->setWordWrap(true);
    hint_->setMinimumHeight(48);
    v->addWidget(hint_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setProperty("role", "muted");
    v->addWidget(status_);

    auto *row = new QHBoxLayout;
    auto *apply = new QPushButton(tr("Save and restart sound servers"));
    auto *reset = new QPushButton(tr("Remove all changes"));
    reset->setToolTip(tr("Deletes the 50-centurion.conf files and restarts the sound servers."));
    auto *close = new QPushButton(tr("Close"));
    row->addWidget(apply);
    row->addWidget(reset);
    row->addStretch(1);
    row->addWidget(close);
    v->addLayout(row);

    connect(apply, &QPushButton::clicked, this, &PipewireConfigDialog::save);
    connect(reset, &QPushButton::clicked, this, &PipewireConfigDialog::removeAll);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int r) {
        if (r >= 0 && r < NOPTS) hint_->setText(tx(OPTS[r].hint));
    });
    load();
}

void PipewireConfigDialog::load() {
    static const char *const WHERE[] = {"pipewire.conf", "pipewire-pulse.conf", "wireplumber settings", "wireplumber ALSA rule"};
    QString text[4];
    for (int s = 0; s < 4; ++s) {
        QFile f(pathOf(Section(s)));
        if (f.open(QIODevice::ReadOnly)) text[s] = QString::fromUtf8(f.read(1 << 20));
    }
    for (int i = 0; i < NOPTS; ++i) {
        const Opt &o = OPTS[i];
        const QString stored = readValue(text[o.section], o);
        auto *set = new QTableWidgetItem;
        set->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        set->setCheckState(stored.isNull() ? Qt::Unchecked : Qt::Checked);
        table_->setItem(i, 0, set);
        auto *name = new QTableWidgetItem(tx(o.name));
        name->setToolTip(tx(o.hint));
        table_->setItem(i, 1, name);
        QWidget *w = editor(o, stored);
        table_->setCellWidget(i, 2, w);
        // Changing a value counts as setting the option.
        auto tick = [set] { set->setCheckState(Qt::Checked); };
        if (auto *c = qobject_cast<QCheckBox *>(w)) connect(c, &QCheckBox::toggled, this, tick);
        else if (auto *n = qobject_cast<QSpinBox *>(w)) connect(n, &QSpinBox::valueChanged, this, tick);
        else if (auto *d = qobject_cast<QDoubleSpinBox *>(w)) connect(d, &QDoubleSpinBox::valueChanged, this, tick);
        else if (auto *b = qobject_cast<QComboBox *>(w)) connect(b, &QComboBox::currentIndexChanged, this, tick);
        else if (auto *e = qobject_cast<QLineEdit *>(w)) connect(e, &QLineEdit::textEdited, this, tick);
        table_->setItem(i, 3, new QTableWidgetItem(o.def[0] ? QString::fromLatin1(o.def) : tr("built-in")));
        table_->setItem(i, 4, new QTableWidgetItem(QString::fromLatin1(WHERE[o.section])));
    }
    table_->resizeColumnToContents(0);
    table_->resizeColumnToContents(2);
    table_->resizeColumnToContents(3);
    table_->selectRow(0);
    hint_->setText(tx(OPTS[0].hint));
}

void PipewireConfigDialog::save() {
    QStringList lines[4];
    int quantum[3] = {32, 1024, 2048};  // min, default, max: the order PipeWire requires
    for (int i = 0; i < NOPTS; ++i) {
        const Opt &o = OPTS[i];
        const QWidget *w = table_->cellWidget(i, 2);
        if (table_->item(i, 0)->checkState() != Qt::Checked) continue;
        const QString val = valueOf(o, w);
        if (val.isNull()) {
            table_->selectRow(i);
            status_->setText(tr("“%1” is not valid: %2").arg(tx(o.name), o.kind == Kind::List ? tr("write rates such as 44100 48000, separated by spaces.")
                                                                                                : tr("write samples/rate, for example 256/48000.")));
            return;
        }
        lines[o.section] << QLatin1String(o.key) + QStringLiteral(" = ") + val;
        const QString k = QLatin1String(o.key);
        if (k == QLatin1String("default.clock.min-quantum")) quantum[0] = val.toInt();
        else if (k == QLatin1String("default.clock.quantum")) quantum[1] = val.toInt();
        else if (k == QLatin1String("default.clock.max-quantum")) quantum[2] = val.toInt();
    }
    if (quantum[0] > quantum[1] || quantum[1] > quantum[2]) {
        status_->setText(tr("The buffer sizes must satisfy: smallest ≤ buffer size ≤ largest (now %1, %2, %3).").arg(quantum[0]).arg(quantum[1]).arg(quantum[2]));
        return;
    }
    const QString wp = block(Wp, lines[Wp]) + block(Alsa, lines[Alsa]);
    const QString texts[3] = {block(Core, lines[Core]), block(Pulse, lines[Pulse]), wp};
    const Section files[3] = {Core, Pulse, Wp};
    for (int i = 0; i < 3; ++i) {
        const QString err = writeFile(pathOf(files[i]), texts[i].isEmpty() ? QString() : QString::fromUtf8(HEADER) + texts[i]);
        if (!err.isEmpty()) { status_->setText(tr("%1: %2").arg(pathOf(files[i]), err)); return; }
    }
    restart(tr("Saved. PipeWire, pipewire-pulse and WirePlumber are restarted."));
}

void PipewireConfigDialog::removeAll() {
    for (Section s : {Core, Pulse, Wp}) {
        const QString err = writeFile(pathOf(s), QString());
        if (!err.isEmpty()) { status_->setText(tr("%1: %2").arg(pathOf(s), err)); return; }
    }
    for (int i = 0; i < NOPTS; ++i) table_->item(i, 0)->setCheckState(Qt::Unchecked);
    restart(tr("Changes removed. The sound servers run with their own defaults again."));
}

void PipewireConfigDialog::restart(const QString &done) {
    status_->setText(tr("Restarting the sound servers…"));
    sysadmin::run(QJsonObject{{QStringLiteral("op"), QStringLiteral("pipewire_restart")}}, this, [this, done](bool ok, const QString &msg) {
        status_->setText(ok ? done : tr("The files are written, but the restart failed: %1").arg(msg));
    });
}
