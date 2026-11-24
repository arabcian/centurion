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
}
