#include "mangohud.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace mangohud {

static const QString BEGIN = QStringLiteral("# >>> centurion overlay (added by Centurion; remove with its switch)");
static const QString END = QStringLiteral("# <<< centurion overlay");

QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/MangoHud/MangoHud.conf");
}

bool available() {
    return !QStandardPaths::findExecutable(QStringLiteral("mangohud")).isEmpty() || QFileInfo(configPath()).absoluteDir().exists();
}

QString overlayFile() {
    const QString d = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return d.isEmpty() || !QDir(d).exists() ? QString() : d + QStringLiteral("/centurion-overlay");
}

QString execCommand(const QString &file) {
    // MangoHud takes the rest of the line as the command: only a path that needs no quoting.
    static const QRegularExpression plain(QStringLiteral("^/[A-Za-z0-9/_.+-]+$"));
    return plain.match(file).hasMatch() ? QStringLiteral("cat ") + file : QStringLiteral("centurion-gamemode OVERLAY");
}

QString overlayLine(const QString &scene, const QString &profileLabel) {
    QStringList parts;
    for (const QString &p : {scene, profileLabel}) if (!p.isEmpty()) parts << p;
    return parts.join(QStringLiteral(" · "));
}

void updateOverlayFile(const QString &line) {
    const QString path = overlayFile();
    if (path.isEmpty()) return;
    if (!enabled()) { QFile::remove(path); return; }
    const QByteArray want = line.toUtf8() + '\n';
    if (QFile f(path); f.open(QIODevice::ReadOnly) && f.read(4096) == want) return;
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly) && f.write(want) == want.size()) f.commit();
}

bool hasOverlay(const QString &conf) { return conf.contains(BEGIN); }

/// `key` set on a line of its own (`key`, `key=…`), ignoring comments and our own block.
static bool names(const QString &conf, const QString &key, QString *value = nullptr) {
    const QRegularExpression re(QStringLiteral("^\\s*%1\\s*(?:=\\s*(\\S*))?\\s*$").arg(QRegularExpression::escape(key)),
                                QRegularExpression::MultilineOption);
    const auto m = re.match(withoutOverlay(conf));
    if (m.hasMatch() && value) *value = m.captured(1);
    return m.hasMatch();
}

QString withOverlay(const QString &conf, const QString &exec) {
    if (hasOverlay(conf)) return conf;
    QStringList block{BEGIN};
    QString legacy;
    if (!names(conf, QStringLiteral("legacy_layout"), &legacy) || (legacy != QLatin1String("0") && legacy != QLatin1String("false"))) {
        block << QStringLiteral("legacy_layout=0");
        for (const char *k : {"gpu_stats", "cpu_stats", "fps", "frame_timing"})
            if (!names(conf, QLatin1String(k))) block << QLatin1String(k);
    }
    block << QStringLiteral("custom_text=Centurion") << QStringLiteral("exec=") + exec << END;
    QString out = conf;
    if (!out.isEmpty() && !out.endsWith('\n')) out += '\n';
    return out + block.join('\n') + '\n';
}

QString withoutOverlay(const QString &conf) {
    const qsizetype b = conf.indexOf(BEGIN);
    if (b < 0) return conf;
    qsizetype e = conf.indexOf(END, b);
    e = e < 0 ? conf.size() : e + END.size();
    if (e < conf.size() && conf.at(e) == '\n') ++e;
    return conf.left(b) + conf.mid(e);
}

static QString read() {
    QFile f(configPath());
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(1 << 20)) : QString();
}

bool enabled() { return hasOverlay(read()); }

bool setEnabled(bool on, QString *err) {
    const QString before = read();
    const QString after = on ? withOverlay(before) : withoutOverlay(before);
    if (after == before) return true;
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QSaveFile f(configPath());
    if (!f.open(QIODevice::WriteOnly) || f.write(after.toUtf8()) < 0 || !f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

} // namespace mangohud
