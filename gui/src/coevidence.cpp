#include "coevidence.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace coevidence {

static QString tr(const char *s) { return QCoreApplication::translate("coevidence", s); }

// AMD SMCA bank numbers 0-6 are a core's own units on Zen client CPUs (LS, IF, L2, DE, -, EX, FP); L3,
// memory controllers and the fabric follow. The threshold counters name the unit directly.
static constexpr int CORE_BANKS = 7;
static const QSet<QString> CORE_UNITS{QStringLiteral("load_store"), QStringLiteral("insn_fetch"), QStringLiteral("combined_unit"),
                                      QStringLiteral("decode_unit"), QStringLiteral("l2_cache"), QStringLiteral("execution_unit"),
                                      QStringLiteral("floating_point")};

QJsonObject Record::toJson() const {
    return {{"source", source}, {"kind", kind}, {"cpu", cpu}, {"bank", bank}, {"unit", unit},
            {"core_unit", coreUnit}, {"ue", uncorrected}, {"text", text.left(300)}};
}

std::optional<Record> Record::fromJson(const QJsonObject &o) {
    Record r{o.value("source").toString(), o.value("kind").toString(), o.value("cpu").toInt(-1), o.value("bank").toInt(-1),
             o.value("unit").toString(), o.value("core_unit").toBool(), o.value("ue").toBool(), o.value("text").toString()};
    if ((r.kind != "mce" && r.kind != "lockup") || r.cpu < -1 || r.cpu > 4095) return std::nullopt;
    return r;
}

std::optional<Record> fromKernelText(const QString &text, const QString &source) {
    Record r;
    r.source = source;
    r.text = text.left(300);
    // Lockups: "soft lockup - CPU#5 stuck", "Watchdog detected hard LOCKUP on cpu 5".
    static const QRegularExpression soft(QStringLiteral(R"(soft lockup - CPU#(\d+))"));
    static const QRegularExpression hard(QStringLiteral(R"(hard LOCKUP on cpu (\d+))"), QRegularExpression::CaseInsensitiveOption);
    for (const auto *re : {&soft, &hard}) {
        if (const auto m = re->match(text); m.hasMatch()) {
            r.kind = QStringLiteral("lockup");
            r.cpu = m.captured(1).toInt();
            return r;
        }
    }
    if (!text.contains(QLatin1String("Hardware Error")) && !text.contains(QLatin1String("Machine Check"))) return std::nullopt;
    // The kernel's own record: "CPU 5: Machine Check: 0 Bank 5: bea0000000000108" (… Exception: …).
    static const QRegularExpression kernel(QStringLiteral(R"(CPU (\d+): Machine Check(?: Exception)?: \S+ Bank (\d+))"));
    // The AMD decoder: "CPU:5 (19:61:2) MC5_STATUS[Over|UE|MiscV|…]: 0x…".
    static const QRegularExpression amd(QStringLiteral(R"(CPU:(\d+) \([0-9a-fA-F:]+\) MC(\d+)_STATUS\[([^\]]*)\])"));
    if (const auto m = kernel.match(text); m.hasMatch()) {
        r.cpu = m.captured(1).toInt();
        r.bank = m.captured(2).toInt();
        r.uncorrected = text.contains(QLatin1String("Exception"));
    } else if (const auto m = amd.match(text); m.hasMatch()) {
        r.cpu = m.captured(1).toInt();
        r.bank = m.captured(2).toInt();
        r.uncorrected = m.captured(3).split('|').contains(QStringLiteral("UE"));
    } else {
        return std::nullopt;  // a machine check without a CPU and bank cannot blame anyone
    }
    r.kind = QStringLiteral("mce");
    r.coreUnit = r.bank >= 0 && r.bank < CORE_BANKS;
    return r;
}

std::optional<Record> fromCounterPath(const QString &path) {
    static const QRegularExpression re(QStringLiteral(R"(/machinecheck(\d+)/([^/]+)/[^/]+/error_count$)"));
    const auto m = re.match(path);
    if (!m.hasMatch()) return std::nullopt;
    Record r;
    r.source = QStringLiteral("watch-counter");
    r.kind = QStringLiteral("mce");
    r.cpu = m.captured(1).toInt();
    r.unit = m.captured(2);
    r.coreUnit = CORE_UNITS.contains(r.unit);
    r.text = tr("corrected-error counter %1 of CPU %2 moved").arg(r.unit).arg(r.cpu);
    return r;
}

Verdict judge(const QList<Record> &records, const QList<cosearch::Core> &cores, const QList<int> &offsets) {
    Verdict v;
    auto coreOf = [&](int cpu) {
        for (int i = 0; i < cores.size(); ++i) if (cores[i].cpus.contains(cpu)) return i;
        return -1;
    };
    QSet<int> named;
    QStringList shared, lockups;
    int mces = 0;
    for (const Record &r : records) {
        const int c = coreOf(r.cpu);
        if (r.kind == "lockup") { if (c >= 0) lockups << QString::number(r.cpu); continue; }
        if (c < 0) continue;
        ++mces;
        if (!r.coreUnit) shared << (r.unit.isEmpty() ? QStringLiteral("bank %1").arg(r.bank) : r.unit);
        else named.insert(c);
    }
    const QString info = lockups.isEmpty() ? QString()
        : tr(" Lockup reports (not used to blame): CPU %1.").arg(lockups.join(", "));
    if (mces == 0) {
        v.summary = tr("No machine-check record names a CPU of this search.") + info;
        return v;
    }
    if (!shared.isEmpty()) {
        v.kind = Verdict::SharedUnit;
        v.summary = tr("Machine check in a shared unit (%1): RAM / fabric as likely as a core; no core blamed.")
                        .arg(QStringList(QSet<QString>(shared.begin(), shared.end()).values()).join(", ")) + info;
        return v;
    }
    if (named.size() > 1) {
        QStringList ids;
        for (int c : named) ids << QStringLiteral("CCD%1/%2").arg(cores[c].ccd).arg(cores[c].slot);
        ids.sort();
        v.kind = Verdict::Ambiguous;
        v.summary = tr("Machine checks on several cores (%1): ambiguous, no core blamed.").arg(ids.join(", ")) + info;
        return v;
    }
    const int c = *named.begin();
    const QString id = QStringLiteral("CCD%1/%2").arg(cores[c].ccd).arg(cores[c].slot);
    if (offsets.value(c) >= 0) {
        v.kind = Verdict::AtStock;
        v.summary = tr("Machine check on %1, which was at stock: not the Curve Optimizer; no core blamed.").arg(id) + info;
        return v;
    }
    v.kind = Verdict::Blamed;
    v.core = c;
    v.cpus = {cores[c].cpus.first()};
    v.summary = tr("Machine check(s) on %1 (at %2) in its own core units: that core is blamed.").arg(id).arg(offsets.value(c)) + info;
    return v;
}

} // namespace coevidence
