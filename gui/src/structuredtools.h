#pragma once
// Strict adapters for known raw tool protocols. Raw output remains available.
#include <QStringList>
#include <QList>
#include <QSet>
#include <QRegularExpression>
#include <optional>
namespace structuredtools {
struct Data { QStringList headers; QList<QStringList> rows; };
inline std::optional<Data> modules(const QString &raw) {
    if (raw.size() > 1024 * 1024) return std::nullopt;
    Data data{{QStringLiteral("Module"), QStringLiteral("Size (bytes)"), QStringLiteral("Use count"), QStringLiteral("Used by")}, {}};
    const QRegularExpression header(QStringLiteral("^Module\\s+Size\\s+Used by\\s*$"));
    const QRegularExpression row(QStringLiteral("^([A-Za-z0-9_]+)\\s+([0-9]+)\\s+([0-9]+)(?:\\s+([A-Za-z0-9_,]+))?\\s*$"));
    bool found = false, started = false;
    QSet<QString> names;
    for (const auto &line : raw.split(QLatin1Char('\n'))) {
        if (!started) { if (line == QStringLiteral("$ lsmod")) started = true; continue; }
        if (line.trimmed().isEmpty()) continue;
        if (!found) { if (!header.match(line).hasMatch()) return std::nullopt; found = true; continue; }
        const auto m = row.match(line);
        bool sizeOk = false, countOk = false;
        m.captured(2).toULongLong(&sizeOk); m.captured(3).toULongLong(&countOk);
        if (!m.hasMatch() || !sizeOk || !countOk || names.contains(m.captured(1)) || data.rows.size() >= 4096) return std::nullopt;
        names.insert(m.captured(1)); data.rows.append(QStringList{m.captured(1), m.captured(2), m.captured(3), m.captured(4)});
    }
    if (!found || data.rows.isEmpty()) return std::nullopt;
    return data;
}
inline std::optional<Data> powerSupply(const QString &raw) {
    if (raw.size() > 1024 * 1024) return std::nullopt;
    Data data{{QStringLiteral("Supply"), QStringLiteral("Kernel field"), QStringLiteral("Raw value")}, {}};
    const QRegularExpression path(QStringLiteral("^\\$ cat /sys/class/power_supply/([A-Za-z0-9_.:-]+)/uevent$"));
    const QRegularExpression field(QStringLiteral("^POWER_SUPPLY_([A-Z0-9_]+)=(.*)$"));
    // Only known, non-identifying readings; units remain the kernel's native units.
    const QSet<QString> allowed{QStringLiteral("TYPE"), QStringLiteral("STATUS"), QStringLiteral("ONLINE"), QStringLiteral("PRESENT"), QStringLiteral("CAPACITY"), QStringLiteral("CYCLE_COUNT"), QStringLiteral("VOLTAGE_NOW"), QStringLiteral("CURRENT_NOW"), QStringLiteral("POWER_NOW"), QStringLiteral("ENERGY_NOW"), QStringLiteral("ENERGY_FULL"), QStringLiteral("ENERGY_FULL_DESIGN"), QStringLiteral("CHARGE_NOW"), QStringLiteral("CHARGE_FULL"), QStringLiteral("CHARGE_FULL_DESIGN"), QStringLiteral("TEMP"), QStringLiteral("HEALTH")};
    QString supply; QSet<QString> seen;
    for (const auto &line : raw.split(QLatin1Char('\n'))) {
        if (line.trimmed().isEmpty()) continue;
        const auto p = path.match(line);
        if (p.hasMatch()) { supply = p.captured(1); continue; }
        if (supply.isEmpty()) return std::nullopt;
        const auto m = field.match(line);
        if (!m.hasMatch()) return std::nullopt;
        if (!allowed.contains(m.captured(1))) continue;
        const QString key = supply + QLatin1Char('/') + m.captured(1);
        if (seen.contains(key) || data.rows.size() >= 4096 || m.captured(2).size() > 128 || m.captured(2).isEmpty()) return std::nullopt;
        seen.insert(key); data.rows.append(QStringList{supply, m.captured(1), m.captured(2)});
    }
    if (data.rows.isEmpty()) return std::nullopt;
    return data;
}
// Extract exactly one captured command section; never execute a command on
// behalf of the structured viewer. Unexpected lines are rejected by adapters.
inline std::optional<QStringList> section(const QString &raw, const QString &command) {
    if (raw.size() > 1024 * 1024) return std::nullopt;
    QStringList rows;
    bool inside = false, found = false;
    for (const auto &line : raw.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("$ "))) {
            inside = line == command;
            if (inside && found) return std::nullopt;
            found = found || inside;
        } else if (inside && !line.trimmed().isEmpty()) {
            if (rows.size() >= 4096 || line.size() > 4096) return std::nullopt;
            rows.append(line.trimmed());
        }
    }
    return found && !rows.isEmpty() ? std::optional<QStringList>(rows) : std::nullopt;
}

inline std::optional<Data> usb(const QString &raw) {
    const auto lines = section(raw, QStringLiteral("$ lsusb"));
    if (!lines) return std::nullopt;
    Data result{{QStringLiteral("Bus"), QStringLiteral("Device"), QStringLiteral("Vendor ID"), QStringLiteral("Product ID")}, {}};
    const QRegularExpression row(QStringLiteral("^Bus ([0-9]{3}) Device ([0-9]{3}): ID ([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})(?: .*)?$"));
    QSet<QString> seen;
    for (const auto &line : *lines) {
        const auto m = row.match(line);
        if (!m.hasMatch()) return std::nullopt;
        const auto key = m.captured(1) + QLatin1Char(':') + m.captured(2);
        if (seen.contains(key)) return std::nullopt;
        seen.insert(key);
        // Do not copy free-form device descriptions into structured exports.
        result.rows.append(QStringList{m.captured(1), m.captured(2), m.captured(3).toLower(), m.captured(4).toLower()});
    }
    return result;
}

inline std::optional<Data> cpuTopology(const QString &raw) {
    const auto lines = section(raw, QStringLiteral("$ lscpu -e"));
    if (!lines || lines->size() < 2) return std::nullopt;
    const QRegularExpression space(QStringLiteral("\\s+"));
    const auto headers = lines->first().split(space, Qt::SkipEmptyParts);
    const QStringList keys{QStringLiteral("CPU"), QStringLiteral("NODE"), QStringLiteral("SOCKET"), QStringLiteral("CORE"), QStringLiteral("ONLINE")};
    if (!headers.contains(QStringLiteral("CPU")) || !headers.contains(QStringLiteral("CORE"))) return std::nullopt;
    QList<int> columns;
    Data result;
    for (const auto &key : keys) {
        const auto column = headers.indexOf(key);
        if (column < 0) continue;
        if (headers.lastIndexOf(key) != column) return std::nullopt;
        columns.append(int(column)); result.headers.append(key);
    }
    QSet<qulonglong> seen;
    const QRegularExpression integer(QStringLiteral("^[0-9]+$"));
    for (qsizetype i = 1; i < lines->size(); ++i) {
        const auto fields = lines->at(i).split(space, Qt::SkipEmptyParts);
        if (fields.size() != headers.size()) return std::nullopt;
        QStringList values;
        for (qsizetype c = 0; c < columns.size(); ++c) {
            const auto value = fields.at(columns.at(c));
            const auto key = result.headers.at(c);
            if (key == QStringLiteral("ONLINE")) {
                if (value != QStringLiteral("yes") && value != QStringLiteral("no") && value != QStringLiteral("-")) return std::nullopt;
            } else if (value != QStringLiteral("-") || key == QStringLiteral("CPU")) {
                bool ok = false;
                const auto n = value.toULongLong(&ok);
                if (!integer.match(value).hasMatch() || !ok) return std::nullopt;
                if (key == QStringLiteral("CPU")) { if (seen.contains(n)) return std::nullopt; seen.insert(n); }
            }
            values.append(value);
        }
        result.rows.append(values);
    }
    return result;
}

}
