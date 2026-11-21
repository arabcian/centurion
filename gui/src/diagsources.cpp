#include "diagsources.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace diagsrc {

static QString tr(const char *s) { return QCoreApplication::translate("diagsrc", s); }

QString read(const QString &path, qint64 limit) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.read(limit)).trimmed();
}

QStringList dir(const QString &path, const QStringList &filter, int max) {
    return QDir(path).entryList(filter, QDir::Dirs | QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDir::Name).mid(0, max);
}

QString stateDir() {
    const QString root = qEnvironmentVariable("XDG_STATE_HOME");
    return (root.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : root) + QStringLiteral("/centurion");
}

QString dataDir() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/centurion"); }

QString bootId() { return read(QStringLiteral("/proc/sys/kernel/random/boot_id"), 80); }

QJsonObject loadJson(const QString &path, qint64 limit) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QString saveJson(const QString &path, const QJsonObject &o) {
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (b.size() > (1 << 20)) return tr("record too large");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return tr("cannot create %1").arg(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return f.errorString();
    if (!f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || f.write(b) != b.size() || !f.commit())
        return tr("could not write %1").arg(path);
    return {};
}

std::optional<LogLine> parseLine(const QString &line) {
    static const QRegularExpression health(QStringLiteral(R"(^(\S+ \S+)\s+boot=(\S+) seq=(\d+)\s+(\S+)\s+(\S+)\s+(.*?)\s+\|\s+(.*)$)"));
    static const QRegularExpression kernel(QStringLiteral(R"(^(\S+ \S+)\s+boot=(\S+) seq=(\d+)\s+(\S+)\s+\[\s*[\d.]+\]\s?(.*)$)"));
    if (const auto m = kernel.match(line); m.hasMatch())
        return LogLine{m.captured(1), m.captured(2).toLower(), m.captured(4).toLower(), {}, {}, m.captured(5), m.captured(3).toULongLong()};
    if (const auto m = health.match(line); m.hasMatch())
        return LogLine{m.captured(1), m.captured(2).toLower(), m.captured(4).toLower(), m.captured(5).toLower(),
                       m.captured(6), m.captured(7), m.captured(3).toULongLong()};
    return std::nullopt;
}

static QList<LogLine> tailOf(const QString &name, int max) {
    QList<LogLine> out;
    for (const QString &file : {name + QStringLiteral(".1"), name}) {
        QFile f(stateDir() + QLatin1Char('/') + file);
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (f.size() > (4 << 20)) { f.seek(f.size() - (4 << 20)); f.readLine(8192); }
        while (!f.atEnd()) {
            if (auto l = parseLine(QString::fromUtf8(f.readLine(8192)).trimmed())) out << *l;
            if (out.size() > max) out.removeFirst();
        }
    }
    return out;
}

QList<LogLine> savedEvents(int max) { return tailOf(QStringLiteral("health.log"), max); }
QList<LogLine> savedKernel(int max) { return tailOf(QStringLiteral("kernel.log"), max); }

QList<LogLine> mergeUnique(const QList<LogLine> &a, const QList<LogLine> &b) {
    QList<LogLine> out;
    QSet<QString> seen;
    for (const QList<LogLine> *l : {&a, &b})
        for (const LogLine &x : *l)
            if (!seen.contains(x.key())) { seen.insert(x.key()); out << x; }
    return out;
}

QString pciClassName(const QString &cls) {
    const QString c = cls.startsWith(QStringLiteral("0x")) ? cls.mid(2, 4) : cls.left(4);
    static const QHash<QString, const char *> names = {
        {"0100", QT_TRANSLATE_NOOP("diagsrc", "SCSI controller")}, {"0106", QT_TRANSLATE_NOOP("diagsrc", "SATA controller")}, {"0108", QT_TRANSLATE_NOOP("diagsrc", "NVMe drive")}, {"0200", QT_TRANSLATE_NOOP("diagsrc", "Ethernet controller")},
        {"0280", QT_TRANSLATE_NOOP("diagsrc", "Wireless network controller")}, {"0300", QT_TRANSLATE_NOOP("diagsrc", "VGA controller")}, {"0302", QT_TRANSLATE_NOOP("diagsrc", "3D controller")}, {"0380", QT_TRANSLATE_NOOP("diagsrc", "Display controller")},
        {"0401", QT_TRANSLATE_NOOP("diagsrc", "Audio device")}, {"0403", QT_TRANSLATE_NOOP("diagsrc", "Audio controller")}, {"0480", QT_TRANSLATE_NOOP("diagsrc", "Multimedia controller")}, {"0500", QT_TRANSLATE_NOOP("diagsrc", "RAM controller")},
        {"0600", QT_TRANSLATE_NOOP("diagsrc", "Host bridge")}, {"0601", QT_TRANSLATE_NOOP("diagsrc", "ISA bridge")}, {"0604", QT_TRANSLATE_NOOP("diagsrc", "PCI bridge")}, {"0680", QT_TRANSLATE_NOOP("diagsrc", "Bridge")}, {"0805", QT_TRANSLATE_NOOP("diagsrc", "SD host controller")},
        {"0880", QT_TRANSLATE_NOOP("diagsrc", "System peripheral")}, {"0c03", QT_TRANSLATE_NOOP("diagsrc", "USB controller")}, {"0c05", QT_TRANSLATE_NOOP("diagsrc", "SMBus controller")}, {"0d11", QT_TRANSLATE_NOOP("diagsrc", "Bluetooth controller")},
        {"1080", QT_TRANSLATE_NOOP("diagsrc", "Encryption controller")}, {"1101", QT_TRANSLATE_NOOP("diagsrc", "Signal processing controller")}, {"1180", QT_TRANSLATE_NOOP("diagsrc", "Signal processing controller")},
        {"1300", QT_TRANSLATE_NOOP("diagsrc", "Non-essential instrumentation")}};
    const char *n = names.value(c.toLower());
    if (n) return tr(n);
    return c.isEmpty() ? tr("PCI device") : tr("PCI class %1").arg(c);
}

QString pciName(const QString &vendor, const QString &device, const QString &cls) {
    // pci.ids is read once per run, only the lines of the vendors asked for: the file is ~1.5 MB.
    static QHash<QString, QString> cache;
    const QString v = vendor.toLower().remove(QStringLiteral("0x")), d = device.toLower().remove(QStringLiteral("0x"));
    const QString key = v + QLatin1Char(':') + d;
    if (cache.contains(key)) return cache.value(key);
    QString name;
    for (const QString &path : {QStringLiteral("/usr/share/hwdata/pci.ids"), QStringLiteral("/usr/share/misc/pci.ids"),
                                QStringLiteral("/usr/share/pci.ids")}) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QString vendorName;
        bool inVendor = false;
        while (!f.atEnd()) {
            const QByteArray line = f.readLine(512);
            if (line.isEmpty() || line.startsWith('#')) continue;
            if (line.at(0) != '\t') {
                if (inVendor) break;  // past our vendor's block
                if (line.startsWith("C ")) break;  // class section
                if (line.left(4).toLower() == v.toLatin1()) { inVendor = true; vendorName = QString::fromUtf8(line.mid(6)).trimmed(); }
                continue;
            }
            if (inVendor && line.size() > 6 && line.at(1) != '\t' && line.mid(1, 4).toLower() == d.toLatin1()) {
                name = QString::fromUtf8(line.mid(7)).trimmed();
                break;
            }
        }
        if (!vendorName.isEmpty()) {
            // "NVIDIA Corporation" + "AD104 [GeForce RTX 4070]": the bracketed marketing name reads best.
            const auto br = QRegularExpression(QStringLiteral(R"(\[([^\]]+)\])")).match(name);
            const QString shortVendor = vendorName.section(QLatin1Char(' '), 0, 0).remove(QLatin1Char(','));
            name = name.isEmpty() ? vendorName + QLatin1Char(' ') + pciClassName(cls).toLower()
                                  : shortVendor + QLatin1Char(' ') + (br.hasMatch() ? br.captured(1) : name);
        }
        break;
    }
    if (name.isEmpty()) name = pciClassName(cls) + QStringLiteral(" (%1:%2)").arg(v, d);
    cache.insert(key, name);
    return name;
}

QString when(qint64 secs) {
    return secs <= 0 ? QStringLiteral("—") : QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString duration(qint64 s) {
    if (s < 0) return QStringLiteral("—");
    if (s < 90) return tr("%1 s").arg(s);
    if (s < 3600) return tr("%1 min").arg(s / 60);
    if (s < 48 * 3600) return tr("%1 h %2 min").arg(s / 3600).arg(s % 3600 / 60);
    return tr("%1 days").arg(s / 86400);
}

}  // namespace diagsrc
