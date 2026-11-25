#include "diagsources.h"

#include <QByteArrayView>
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
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

static bool openRegular(QFile &f, const QString &path, bool *missing = nullptr) {
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (missing) *missing = fd < 0 && errno == ENOENT;
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || !f.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd); return false;
    }
    return true;
}

std::optional<QByteArray> readRecord(const QString &path, qint64 limit) {
    QFile f;
    if (limit < 0 || limit > (32LL << 20) || !openRegular(f, path)) return std::nullopt;
    struct stat before{}, after{};
    if (::fstat(f.handle(), &before) != 0 || before.st_size < 0 || before.st_size > limit) return std::nullopt;
    const auto data = f.read(limit + 1);
    if (f.error() != QFileDevice::NoError || data.size() != before.st_size || !f.atEnd()
        || ::fstat(f.handle(), &after) != 0 || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        return std::nullopt;
    }
    return data;
}

QJsonObject loadJson(const QString &path, qint64 limit) {
    const auto data = readRecord(path, limit);
    return data ? QJsonDocument::fromJson(*data).object() : QJsonObject{};
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
    if (const auto m = kernel.match(line); m.hasMatch()) {
        bool ok = false; const auto seq = m.captured(3).toULongLong(&ok);
        if (!ok) return std::nullopt;
        return LogLine{m.captured(1), m.captured(2).toLower(), m.captured(4).toLower(), {}, {}, m.captured(5), seq};
    }
    if (const auto m = health.match(line); m.hasMatch()) {
        bool ok = false; const auto seq = m.captured(3).toULongLong(&ok);
        if (!ok) return std::nullopt;
        return LogLine{m.captured(1), m.captured(2).toLower(), m.captured(4).toLower(), m.captured(5).toLower(),
                       m.captured(6), m.captured(7), seq};
    }
    return std::nullopt;
}

std::array<LogFileCoverage, 2> scanSavedLog(
    SavedLog source, qint64 maxBytesPerFile, const std::function<bool(const QString &, bool)> &visit,
    const QString &day) {
    std::array<LogFileCoverage, 2> files{};
    maxBytesPerFile = std::clamp<qint64>(maxBytesPerFile, 0, 16LL << 20);
    if (!maxBytesPerFile) return files;
    const bool health = source == SavedLog::Health;
    const QString name = health ? QStringLiteral("health.log") : QStringLiteral("kernel.log");
    const QByteArray dayPrefix = day.isEmpty() ? QByteArray() : (day + QLatin1Char(' ')).toLatin1();
    for (int index = 0; index < 2; ++index) {
        const bool rotated = index == 0;
        const QString file = rotated ? name + QStringLiteral(".1") : name;
        auto &coverage = files.at(index);
        QFile f;
        bool missing = false;
        if (!openRegular(f, stateDir() + QLatin1Char('/') + file, &missing)) {
            coverage.state = missing ? LogFileState::Missing : LogFileState::Unreadable;
            continue;
        }
        coverage.state = LogFileState::Unreadable;
        const qint64 size = f.size(), start = std::max<qint64>(0, size - maxBytesPerFile);
        if (size < 0) continue;
        char previous = '\n';
        if (start > 0 && (!f.seek(start - 1) || !f.getChar(&previous))) continue;
        const auto bytes = f.read(size - start);
        if (f.error() != QFileDevice::NoError || bytes.size() != size - start) continue;
        coverage.state = LogFileState::Read;
        coverage.omittedBytes = start;
        coverage.incompleteTail = !bytes.isEmpty() && !bytes.endsWith('\n');
        qsizetype pos = 0;
        if (previous != '\n') {
            const auto firstNewline = bytes.indexOf('\n');
            if (firstNewline < 0) continue;
            pos = firstNewline + 1;
        }
        while (pos < bytes.size()) {
            const auto end = bytes.indexOf('\n', pos);
            if (end < 0) break;
            const QByteArrayView record(bytes.constData() + pos, end - pos);
            pos = end + 1;
            if (!dayPrefix.isEmpty() && !record.startsWith(dayPrefix)) continue;
            if (record.size() > 8192 || !visit(QString::fromUtf8(record.data(), record.size()).trimmed(), rotated))
                ++coverage.rejectedLines;
        }
    }
    return files;
}

LogTail savedLog(SavedLog source, int max, qint64 maxBytesPerFile, const QString &day) {
    LogTail out;
    max = std::clamp(max, 0, 10000);
    if (!max) return out;
    out.files = scanSavedLog(source, maxBytesPerFile, [&](const QString &record, bool rotated) {
        auto line = parseLine(record);
        if (!line || (source == SavedLog::Health) != !line->kind.isEmpty()) return false;
        line->rotated = rotated;
        out.lines << std::move(*line);
        if (out.lines.size() > max) {
            out.lines.removeFirst();
            ++out.omittedLines;
        }
        return true;
    }, day);
    return out;
}

QList<LogLine> savedEvents(int max) { return savedLog(SavedLog::Health, max).lines; }
QList<LogLine> savedKernel(int max) { return savedLog(SavedLog::Kernel, max).lines; }

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
