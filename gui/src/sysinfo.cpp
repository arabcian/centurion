#include "sysinfo.h"
#include "telemetrycache.h"
#include "sensorincarnation.h"
#include <time.h>
#include <cmath>
#include <QElapsedTimer>
#include <QMutex>
#include <QHash>
#include <QMap>
#include "platformprofile.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <fstream>
#include <sys/utsname.h>

namespace sysinfo {

static const QString SEP = QStringLiteral("  ·  ");
static Opt rd(const QString &p) { return pp::readText(p); }
// Numbers parse straight from the bytes: no UTF-16 round trip per attribute.
static std::optional<long long> rdInt(const QString &p) {
    const auto s = pp::readRaw(p);
    bool ok = false;
    const long long v = s ? s->trimmed().toLongLong(&ok) : 0;
    return ok ? std::optional(v) : std::nullopt;
}
static Opt joined(const QStringList &l) { return l.isEmpty() ? Opt() : Opt(l.join(SEP)); }

Opt dmi(const QString &f) { return rd(QStringLiteral("/sys/class/dmi/id/") + f); }

bool supportedMachine(QString *reason) {
    // Word-anchored: "Legion Pro 7", "LOQ 15IRH8", "IdeaPad Gaming 3" — not "Legionnaire".
    static const QRegularExpression family(QStringLiteral("\\b(legion|loq|ideapad gaming)\\b"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QString vendor = dmi(QStringLiteral("sys_vendor")).value_or(QString());
    const QString version = dmi(QStringLiteral("product_version")).value_or(QString());
    const QString fam = dmi(QStringLiteral("product_family")).value_or(QString());
    const QString shown = !version.isEmpty() ? version : !fam.isEmpty() ? fam : dmi(QStringLiteral("product_name")).value_or(QString());
    const bool lenovo = vendor.compare(QLatin1String("LENOVO"), Qt::CaseInsensitive) == 0;
    // China-market names that may omit "Legion" (Y9000P, R9000K, GeekPro G5000…);
    // same list as the helpers (crates/centurion-helpers/src/machine.rs, CN_MODELS).
    static const QRegularExpression cn(QStringLiteral("(^|[^A-Za-z0-9])(Y9000|R9000|Y7000|R7000|G5000)"),
                                       QRegularExpression::CaseInsensitiveOption);
    if (lenovo && (family.match(version).hasMatch() || family.match(fam).hasMatch()
                   || cn.match(version).hasMatch() || cn.match(fam).hasMatch())) return true;
    if (reason) {
        QString what = QStringList{vendor, shown}.filter(QRegularExpression(QStringLiteral("\\S"))).join(' ');
        if (what.isEmpty()) what = QStringLiteral("no DMI information");
        *reason = QStringLiteral("This computer (%1) is not supported.\n\nCenturion runs only on "
                                 "Lenovo Legion, LOQ and IdeaPad Gaming laptops: it writes Lenovo-specific "
                                 "firmware and embedded-controller settings.").arg(what);
    }
    return false;
}

bool knownModel() {
    // LenovoLegionToolkit Compatibility.cs AllowedModelsPrefix, same substring
    // match (case-insensitive, anywhere in the model). ThinkBook is left out:
    // supportedMachine() refuses it before this is asked.
    static const char *const prefixes[] = {
        "8APU1",                                                    // Legion Go
        "17ACH", "17ARH", "17ITH", "17IMH", "17ITL",
        "16ACH", "16ADR", "16AFR", "16AHP", "16APH", "16ARH", "16ARP", "16ARX",
        "16IAH", "16IAX", "16IMH", "16IRH", "16IRX", "16ITH",
        "18IAX", "NX",
        "15ACH", "15AHP", "15ALC", "15IPH", "15AKP", "15APH", "15ARH", "15ARP",
        "15ASH", "15IAH", "15IAX", "15IHU", "15IMH", "15IRH", "15IRX", "15ITH",
        "14AHP", "14APH", "14IRP", "14AKP", "14IRH", "14IAP",
        "G5000", "R9000", "R7000", "Y9000", "Y7000",                // China market
        "17IR", "15IR", "15IC", "15IK",                             // limited compatibility
    };
    for (const char *f : {"product_version", "product_family"}) {
        const QString name = dmi(QLatin1String(f)).value_or(QString());
        for (const char *p : prefixes)
            if (name.contains(QLatin1String(p), Qt::CaseInsensitive)) return true;
    }
    return false;
}

QString reportInfo() {
    QStringList out;
    auto add = [&out](const char *k, const Opt &v) { out << QStringLiteral("%1: %2").arg(QLatin1String(k), v.value_or(QStringLiteral("-"))); };
    for (const char *f : {"sys_vendor", "product_name", "product_version", "product_family", "product_sku",
                          "board_name", "bios_version", "bios_date", "ec_firmware_release"})
        add(f, dmiClean(QLatin1String(f)));
    add("cpu", cpuModel());
    add("kernel", kernel());
    out << QStringLiteral("centurion: %1").arg(QLatin1String(CENTURION_VERSION));
    return out.join('\n');
}

bool fnLockInverted() {
    for (const char *f : {"product_version", "product_family"})
        if (dmi(QLatin1String(f)).value_or(QString()).contains(QLatin1String("ideapad gaming"), Qt::CaseInsensitive)) return true;
    return false;
}

Opt dmiClean(const QString &f) {
    static const QSet<QString> junk{"", "none", "not specified", "n/a", "to be filled by o.e.m.", "default string"};
    auto v = dmi(f);
    if (!v || junk.contains(v->trimmed().toLower())) return std::nullopt;
    return v->trimmed();
}

/// First line starting with `prefix` (case-insensitive). `field < 0` → the part
/// after ':', otherwise the whitespace-separated field index.
///
/// Read with std::ifstream, not QFile: procfs files report size 0, and
/// QFile::atEnd() trusts the size, so a `while (!f.atEnd())` loop never ran
/// and /proc/cpuinfo looked empty ("unknown CPU", no Memory row).
static Opt grepFile(const QString &path, const QString &prefix, int field) {
    std::ifstream in(path.toStdString());
    std::string raw;
    while (std::getline(in, raw)) {
        const QString line = QString::fromStdString(raw);
        if (!line.startsWith(prefix, Qt::CaseInsensitive)) continue;
        if (field < 0) return line.section(':', 1).trimmed();
        return line.simplified().section(' ', field, field);
    }
    return std::nullopt;
}

Opt cpuModel() { return grepFile(QStringLiteral("/proc/cpuinfo"), QStringLiteral("model name"), -1); }

CpuVendor cpuVendor() {
    static const CpuVendor v = [] {
        const QString o = qEnvironmentVariable("CENTURION_CPU_VENDOR").toLower();
        if (o == "amd") return CpuVendor::Amd;
        if (o == "intel") return CpuVendor::Intel;
        const QString id = grepFile(QStringLiteral("/proc/cpuinfo"), QStringLiteral("vendor_id"), -1).value_or(QString());
        if (id == "GenuineIntel") return CpuVendor::Intel;
        if (id == "AuthenticAMD" || id == "HygonGenuine") return CpuVendor::Amd;
        return CpuVendor::Other;
    }();
    return v;
}

Opt ramTotal() {
    auto kib = grepFile(QStringLiteral("/proc/meminfo"), QStringLiteral("MemTotal:"), 1);
    bool ok = false;
    double v = kib ? kib->toDouble(&ok) : 0;
    return ok ? Opt(QStringLiteral("%1 GiB").arg(v / (1024.0 * 1024.0), 0, 'f', 1)) : std::nullopt;
}

Opt kernel() {
    utsname u{};
    return uname(&u) == 0 ? Opt(QString::fromUtf8(u.release)) : std::nullopt;
}

Opt biosInfo() {
    QStringList bits;
    auto vendor = dmiClean("bios_vendor"), version = dmiClean("bios_version");
    if (vendor && version) bits << *vendor + ' ' + *version;
    else if (vendor || version) bits << vendor.value_or(version.value_or(QString()));
    if (auto d = dmiClean("bios_date")) bits << '(' + *d + ')';
    if (auto r = dmiClean("bios_release")) bits << '[' + *r + ']';
    const QString t = bits.join(' ').trimmed();
    return t.isEmpty() ? Opt() : Opt(t);
}

Opt systemInfo() {
    QStringList parts;
    if (auto v = dmiClean("sys_vendor")) parts << *v;
    if (auto f = dmiClean("product_family")) parts << *f;
    else if (auto v = dmiClean("product_version")) parts << *v;  // the model, when the family is not filled in
    const QString t = parts.join(' ');  // no SKU: a long code that says nothing the model does not
    return t.isEmpty() ? Opt() : Opt(t);
}

/// Directory entries that only change on hot-plug (power supplies, powercap
/// zones, DRM cards), filtered once by `keep` and re-listed at most every 30 s.
/// Thread-safe: the Live getters run on a worker thread.
static QStringList cachedDirs(const QString &dir, bool (*keep)(const QString &path, const QString &entry)) {
    struct Entry { QStringList paths; std::optional<telemetry::Tick> scanned; };
    static QMutex mu;
    static QHash<QString, Entry> cache;
    const QMutexLocker lock(&mu);
    const QString key = dir + QChar(0) + QString::number(quintptr(keep));
    Entry &e = cache[key];
    const auto now = presentationClock();
    if (!e.scanned || now < *e.scanned || now - *e.scanned >= telemetry::Tick(30000)) {
        e.paths.clear();
        const QDir d(dir);
        for (const QString &n : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name))
            if (const QString p = d.filePath(n); keep(p, n)) e.paths << p;
        e.scanned = now;
    }
    return e.paths;
}

// ── hwmon ───────────────────────────────────────────────────────────────────

/// One `<kind>N_input` file with its N and lower-cased label (both fixed for the
/// chip's lifetime, so they are read once per scan, not once per poll).
struct Input { QString path; int index; QString label; };
struct Chip { QString name, path; QList<Input> temp, fan, power; std::optional<sensorincarnation::Identity> identity; QString canonical; };
static bool chipCurrent(const Chip &c) {
    return !c.canonical.isEmpty() && QFileInfo(c.path).canonicalFilePath() == c.canonical &&
        sensorincarnation::same(QFile::encodeName(c.path).constData(),c.identity);
}
// A replaced hwmon number must not inherit the old driver's labels/units.
static std::optional<long long> chipRead(const Chip &c, const QString &path) {
    if (!chipCurrent(c)) return {};
    const auto value = rdInt(path);
    return chipCurrent(c) ? value : std::nullopt;
}

static QList<Chip> scanChips();
// hwmon chips only appear/disappear on module load or hot-plug, and a chip's
// sensor files and labels never change while it exists: the directory walk,
// the three globs per chip and every *_label read used to run for each 2 s
// Live poll; now they run once per 30 s rescan and a poll only reads values.
// The Live getters run on a worker thread (HomeTab::refreshLive), so the
// shared cache is guarded.
static QList<Chip> chips() {
    static QMutex mu;
    static QList<Chip> cache;
    static std::optional<telemetry::Tick> scanned;
    const QMutexLocker lock(&mu);
    const auto now = presentationClock();
    if (!scanned || now < *scanned || now - *scanned >= telemetry::Tick(30000) || cache.isEmpty() ||
        std::any_of(cache.cbegin(),cache.cend(),[](const Chip &c){return !chipCurrent(c);})) {
        cache = scanChips(); scanned = now;
    }
    return cache;  // implicitly shared: a copy is a refcount bump
}

/// N of ".../<kind>N_input" (0 if absent). Plain scan instead of a shared
/// static QRegularExpression: runs on a worker thread, and in sort comparators.
static int trailingIndex(const QString &file) {
    static const QLatin1String suffix("_input");
    if (!file.endsWith(suffix)) return 0;
    const qsizetype end = file.size() - suffix.size();
    qsizetype b = end;
    while (b > 0 && file.at(b - 1).isDigit()) --b;
    return b < end ? QStringView(file).mid(b, end - b).toInt() : 0;
}

/// Sensor files `<kind>N_input` sorted by N (not lexically: temp10 > temp2), one
/// directory listing per chip for all three kinds.
static void scanInputs(Chip &c) {
    const QStringList all = QDir(c.path).entryList({QStringLiteral("temp*_input"), QStringLiteral("fan*_input"),
                                                    QStringLiteral("power*_input")}, QDir::Files | QDir::System);
    for (const QString &f : all) {
        Input in{c.path + '/' + f, trailingIndex(f), QString()};
        in.label = rd(in.path.chopped(6) + QStringLiteral("_label")).value_or(QString()).trimmed().toLower();
        (f.startsWith(QLatin1String("temp")) ? c.temp : f.startsWith(QLatin1String("fan")) ? c.fan : c.power).append(in);
    }
    for (QList<Input> *l : {&c.temp, &c.fan, &c.power})
        std::sort(l->begin(), l->end(), [](const Input &a, const Input &b) { return a.index < b.index; });
}

static QList<Chip> scanChips() {
    QList<Chip> out;
    const QDir d(QStringLiteral("/sys/class/hwmon"));
    for (const QString &e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name)) {
        const QString p = d.filePath(e);
        const auto identity = sensorincarnation::identify(QFile::encodeName(p).constData());
        const auto canonical = QFileInfo(p).canonicalFilePath();
        if (!identity || canonical.isEmpty()) continue;
        if (auto n = rd(p + "/name"); n && !n->isEmpty()) {
            Chip c{*n, p, {}, {}, {}, identity, canonical};
            scanInputs(c);
            if (chipCurrent(c)) out.append(std::move(c));
        }
    }
    return out;
}

static const Input *temperatureInput(const Chip &chip) {
    if (chip.temp.isEmpty()) return nullptr;
    const Input *chosen = &chip.temp.first();
    for (const Input &f : chip.temp)
        if (f.label.contains(QLatin1String("tctl")) || f.label.contains(QLatin1String("tdie"))
            || f.label.contains(QLatin1String("package"))) { chosen = &f; break; }
    return chosen;
}
static std::optional<double> chipTemp(const Chip &chip) {
    const auto *chosen = temperatureInput(chip);
    if (!chosen) return std::nullopt;
    auto v = chipRead(chip, chosen->path);
    return v ? std::optional(*v / 1000.0) : std::nullopt;
}

static std::optional<PresentationReading> read_cpuTemp() {
    const auto cs = chips();
    for (const char *want : {"k10temp", "zenpower", "coretemp"}) {
        for (const Chip &c : cs) {
            if (c.name != QLatin1String(want)) continue;
            auto main = chipTemp(c);
            if (!main) continue;
            QString t = QStringLiteral("%1 °C").arg(*main, 0, 'f', 0);
            long long hottest = -1;
            for (const Input &f : c.temp) {
                // coretemp: "Core N" per physical core; show the hottest next to the package.
                if (f.label.startsWith(QLatin1String("core "))) { if (auto v = chipRead(c, f.path)) hottest = std::max(hottest, *v); continue; }
                if (!f.label.startsWith(QLatin1String("tccd"))) continue;
                if (auto v = chipRead(c, f.path))
                    t += SEP + QString(f.label).replace(QLatin1String("tccd"), QLatin1String("CCD")) + QStringLiteral(" %1°C").arg(*v / 1000.0, 0, 'f', 0);
            }
            if (hottest >= 0) t += SEP + QStringLiteral("hottest core %1°C").arg(hottest / 1000.0, 0, 'f', 0);
            if (!chipCurrent(c)) continue;
            PresentationReading r{t, {{QStringLiteral("cpu_temp_c"), *main}}};
            r.metrics.append({QStringLiteral("cpu_temp_c"), temperatureInput(c)->path, QFileInfo(c.path + "/device").canonicalFilePath(), MetricUnit::Celsius, *main});
            return r;
        }
    }
    return std::nullopt;
}

std::optional<double> cpuTempC() {
    const auto cs = chips();
    for (const char *want : {"k10temp", "zenpower", "coretemp"})
        for (const Chip &c : cs)
            if (c.name == QLatin1String(want))
                if (auto t = chipTemp(c)) return t;
    return std::nullopt;
}

static bool presentationHwmonAllowed(const Chip &c);

static std::optional<PresentationReading> read_fans() {
    // Several drivers (lenovo_wmi_other, legion hwmon, …) report the same physical
    // fans under the same index. One reading per index, from the chip with the
    // most fans first, so a fan only one driver knows (fan4) still shows up.
    QList<Chip> cs = chips();
    std::stable_sort(cs.begin(), cs.end(), [](const Chip &a, const Chip &b) { return a.fan.size() > b.fan.size(); });
    QMap<int, long long> rpmByIndex;
    QMap<int, NumericMetric> provenance;
    for (const Chip &c : std::as_const(cs)) {
        if (!presentationHwmonAllowed(c)) continue;
        for (const Input &f : c.fan)
            if (!rpmByIndex.contains(f.index) || rpmByIndex.value(f.index) == 0)
                if (auto rpm = chipRead(c, f.path); rpm && *rpm >= 0) {
                    rpmByIndex.insert(f.index, *rpm);
                    provenance.insert(f.index, {QStringLiteral("fan%1_rpm").arg(f.index), f.path, QFileInfo(c.path + "/device").canonicalFilePath(), MetricUnit::Rpm, double(*rpm)});
                }
    }
    QStringList l; QMap<QString, double> values;
    for (auto it = rpmByIndex.cbegin(); it != rpmByIndex.cend(); ++it) {
        if (it.value() > 0) l << QStringLiteral("Fan %1 %2 RPM").arg(it.key()).arg(it.value());
        values.insert(QStringLiteral("fan%1_rpm").arg(it.key()), double(it.value()));
    }
    if (values.isEmpty()) return std::nullopt;
    PresentationReading r{joined(l), values}; r.metrics = provenance.values(); return r;
}

static std::optional<PresentationReading> read_storage() {
    QStringList l; QMap<QString, double> values;
    int n = 0; QList<NumericMetric> metrics;
    for (const Chip &c : chips()) {
        if (c.name != QLatin1String("nvme")) continue;
        ++n;
        if (auto t = chipTemp(c)) {
            l << QStringLiteral("NVMe %1 %2°C").arg(n).arg(*t, 0, 'f', 0);
            const auto key = QStringLiteral("nvme%1_temp_c").arg(n);
            values.insert(key, *t);
            metrics.append({key, temperatureInput(c)->path, QFileInfo(c.path + "/device").canonicalFilePath(), MetricUnit::Celsius, *t});
        }
    }
    if (values.isEmpty()) return std::nullopt;
    PresentationReading r{joined(l), values}; r.metrics = metrics; return r;
}

// A display-class PCI sensor read may runtime-resume the GPU. Only sample
// when runtime PM reports active; unknown/suspended devices are not queried.
// This is a best-effort gate, not an atomic PM reservation.
static bool presentationHwmonAllowed(const Chip &c) {
    if (!chipCurrent(c)) return false;
    const QString device = c.path + QStringLiteral("/device");
    const auto klass = rd(device + QStringLiteral("/class"));
    if (!klass || !klass->startsWith(QLatin1String("0x03"))) {
        // Known GPU hwmon without PCI evidence: don't assume it is integrated.
        return c.name != QLatin1String("amdgpu") && c.name != QLatin1String("nouveau") && c.name != QLatin1String("nvidia");
    }
    return rd(device + QStringLiteral("/power/runtime_status")) == QStringLiteral("active") && chipCurrent(c);
}

static std::optional<PresentationReading> read_power() {
    QStringList l; QMap<QString, double> values; QList<NumericMetric> metrics;
    for (const Chip &c : chips()) {
        if (!presentationHwmonAllowed(c)) continue;
        for (const Input &f : c.power) {
            const auto key = QStringLiteral("power_%1_w").arg(c.name);
            // Text shows the first four reported rails; numeric API keeps the
            // first rail for each hwmon name, as before. Never read a rail twice.
            if (l.size() >= 4 && values.contains(key)) continue;
            if (auto uw = chipRead(c, f.path); uw && *uw > 0) {
                if (l.size() < 4) l << QStringLiteral("%1 %2 W").arg(c.name).arg(*uw / 1e6, 0, 'f', 1);
                if (!values.contains(key)) { values.insert(key, *uw / 1e6); metrics.append({key,f.path,QFileInfo(c.path + "/device").canonicalFilePath(),MetricUnit::Watts,*uw / 1e6}); }
            }
        }
    }
    if (values.isEmpty()) return std::nullopt;
    PresentationReading r{joined(l), values}; r.metrics = metrics; return r;
}

static std::optional<PresentationReading> read_igpu() {
    for (const Chip &c : chips()) {
        if (c.name != "amdgpu" || !presentationHwmonAllowed(c)) continue;
        QStringList b; QMap<QString, double> values;
        if (auto t = chipTemp(c)) { b << QStringLiteral("%1°C").arg(*t, 0, 'f', 0); values.insert(QStringLiteral("igpu_temp_c"), *t); }
        if (!c.power.isEmpty())
            if (auto uw = chipRead(c, c.power.first().path)) b << QStringLiteral("%1W").arg(*uw / 1e6, 0, 'f', 1);
        if (auto hz = chipRead(c, c.path + "/freq1_input")) b << QStringLiteral("%1MHz").arg(*hz / 1000000);
        if (b.isEmpty() || !chipCurrent(c)) continue;
        PresentationReading r{joined(b), values};
        if (values.contains(QStringLiteral("igpu_temp_c"))) r.metrics.append({QStringLiteral("igpu_temp_c"),temperatureInput(c)->path,QFileInfo(c.path + "/device").canonicalFilePath(),MetricUnit::Celsius,values.value(QStringLiteral("igpu_temp_c"))});
        return r;
    }
    // Intel iGPU (i915 / xe): no hwmon temp on most parts, the actual GT clock is enough.
    const QStringList cards = cachedDirs(QStringLiteral("/sys/class/drm"), [](const QString &p, const QString &e) {
        return e.startsWith(QLatin1String("card")) && !e.contains('-') && rd(p + QStringLiteral("/device/vendor")) == QStringLiteral("0x8086");
    });
    for (const QString &card : cards) {
        if (rd(card + QStringLiteral("/device/power/runtime_status")) != QStringLiteral("active")) continue;
        auto mhz = rdInt(card + "/gt/gt0/rps_act_freq_mhz");                    // i915
        if (!mhz) mhz = rdInt(card + "/device/tile0/gt0/freq0/act_freq");        // xe
        if (!mhz) continue;
        QStringList b{QStringLiteral("%1MHz").arg(*mhz)};
        if (auto max = rdInt(card + "/gt/gt0/rps_max_freq_mhz")) b << QStringLiteral("max %1MHz").arg(*max);
        return PresentationReading{joined(b), {}};
    }
    return std::nullopt;
}

/// The system battery (type Battery, not a peripheral's scope=Device one); empty when there is none.
static QString batteryDir() {
    const QStringList bats = cachedDirs(QStringLiteral("/sys/class/power_supply"), [](const QString &p, const QString &) {
        return rd(p + QStringLiteral("/type")) == QStringLiteral("Battery") && rd(p + QStringLiteral("/scope")).value_or(QString()) != QStringLiteral("Device");
    });
    return bats.isEmpty() ? QString() : bats.first();
}

bool batteryPresent() { return !batteryDir().isEmpty(); }

BatteryState batteryState() {
    BatteryState b;
    const QString p = batteryDir();
    if (p.isEmpty()) return b;
    b.present = true;
    b.status = rd(p + "/status").value_or(QString());
    if (auto c = rdInt(p + "/capacity")) b.capacity = int(*c);
    if (auto c = rdInt(p + "/cycle_count"); c && *c > 0) b.cycles = int(*c);
    const auto uv = rdInt(p + "/voltage_now");
    auto vmin = rdInt(p + "/voltage_min_design");
    if (!vmin) vmin = uv;
    auto wh = [&](const char *e, const char *c, std::optional<long long> volt) -> double {
        if (auto x = rdInt(p + '/' + QLatin1String(e)); x && *x >= 0) return *x / 1e6;
        if (auto x = rdInt(p + '/' + QLatin1String(c)); x && *x >= 0 && volt && *volt > 0) return *x / 1e6 * (*volt / 1e6);
        return -1;
    };
    b.energyWh = wh("energy_now", "charge_now", vmin);
    b.fullWh = wh("energy_full", "charge_full", vmin);
    b.designWh = wh("energy_full_design", "charge_full_design", vmin);
    if (auto pw = rdInt(p + "/power_now"); pw && *pw > 0) b.powerW = *pw / 1e6;
    else if (auto cur = rdInt(p + "/current_now"); cur && uv && *cur != 0) b.powerW = std::abs(double(*cur)) / 1e6 * (*uv / 1e6);
    return b;
}

int BatteryState::minutesLeft() const {
    if (powerW < 0.5 || energyWh < 0) return -1;
    if (status == QLatin1String("Discharging")) return int(energyWh / powerW * 60);
    if (status == QLatin1String("Charging") && fullWh > energyWh) return int((fullWh - energyWh) / powerW * 60);
    return -1;
}

QString formatMinutes(int m) {
    if (m < 0) return QString();
    return m >= 60 ? QStringLiteral("%1 h %2 min").arg(m / 60).arg(m % 60, 2, 10, QLatin1Char('0')) : QStringLiteral("%1 min").arg(m);
}

static std::optional<PresentationReading> read_battery() {
    const QString p = batteryDir();
    if (p.isEmpty()) return std::nullopt;
    const BatteryState st = batteryState();
    QStringList b;
    if (st.capacity >= 0) b << QString::number(st.capacity) + '%';
    if (!st.status.isEmpty()) b << st.status;
    if (st.fullWh >= 0 && st.designWh > 0) b << QStringLiteral("health %1%").arg(100.0 * st.fullWh / st.designWh, 0, 'f', 0);
    // Draw and time left / to full at the current rate.
    if (st.powerW >= 0.1 && (st.status == QLatin1String("Discharging") || st.status == QLatin1String("Charging")))
        b << QStringLiteral("%1 W").arg(st.powerW, 0, 'f', 1);
    if (const int m = st.minutesLeft(); m >= 0 && m < 48 * 60)
        b << (st.status == QLatin1String("Charging") ? QStringLiteral("full in ~") : QStringLiteral("~")) + formatMinutes(m)
                 + (st.status == QLatin1String("Charging") ? QString() : QStringLiteral(" left"));
    else if (auto uv = rdInt(p + "/voltage_now")) b << QStringLiteral("%1V").arg(*uv / 1e6, 0, 'f', 2);
    QMap<QString, double> values;
    if (st.capacity >= 0) values.insert(QStringLiteral("battery_percent"), st.capacity);
    if (st.powerW >= 0) values.insert(QStringLiteral("battery_power_w"), st.powerW);
    if (st.fullWh > 0 && st.designWh > 0) values.insert(QStringLiteral("battery_health_percent"), 100.0 * st.fullWh / st.designWh);
    if (b.isEmpty() && values.isEmpty()) return std::nullopt;
    PresentationReading r{joined(b), values};
    for (auto i=values.cbegin(); i!=values.cend(); ++i) r.metrics.append({i.key(),p,QFileInfo(p).canonicalFilePath(),i.key()==QStringLiteral("battery_power_w") ? MetricUnit::Watts : MetricUnit::Percent,i.value()});
    return r;
}

// Shared display slots for Home and both D-Bus APIs. CLOCK_BOOTTIME includes
// suspend. CPU fan guard and direct battery/transaction reads remain independent.
telemetry::Tick presentationClock() {
    timespec ts{};
    if (::clock_gettime(CLOCK_BOOTTIME, &ts) == 0)
        return std::chrono::duration_cast<telemetry::Tick>(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));
    return std::chrono::duration_cast<telemetry::Tick>(std::chrono::steady_clock::now().time_since_epoch());
}
bool PresentationReading::fresh() const {
    const auto now = presentationClock();
    return telemetry::freshAt(quality, captured, ttl, now);
}
#define DISPLAY_SENSOR(name, lifetime) \
    static PresentationReading name##Presentation() { \
        static telemetry::Slot<PresentationReading> slot; \
        const auto sample = slot.read(telemetry::Tick(lifetime), presentationClock, read_##name); \
        auto value = sample.value.value_or(PresentationReading{}); \
        static const auto boot = rd(QStringLiteral("/proc/sys/kernel/random/boot_id")).value_or(QString()); \
        value.bootId = boot; value.generation = sample.generation; \
        value.quality = sample.quality; value.captured = sample.captured; value.ttl = telemetry::Tick(lifetime); \
        return value; \
    } \
    Opt name() { const auto value = name##Presentation(); return value.fresh() ? value.text : Opt{}; }
DISPLAY_SENSOR(cpuTemp, 1500)
DISPLAY_SENSOR(fans, 1500)
DISPLAY_SENSOR(storage, 1500)
DISPLAY_SENSOR(power, 1500)
DISPLAY_SENSOR(igpu, 1500)
DISPLAY_SENSOR(battery, 2000)
#undef DISPLAY_SENSOR

PresentationReading presentationReading(PresentationMetric metric) {
    switch (metric) {
    case PresentationMetric::CpuTemp: return cpuTempPresentation();
    case PresentationMetric::Fans: return fansPresentation();
    case PresentationMetric::Storage: return storagePresentation();
    case PresentationMetric::Power: return powerPresentation();
    case PresentationMetric::Igpu: return igpuPresentation();
    case PresentationMetric::Battery: return batteryPresentation();
    }
    return {};
}

QMap<QString, QString> PresentationSnapshot::text() const {
    QMap<QString, QString> result;
    for (const auto &[key, reading] : readings)
        if (reading.fresh() && reading.text && !reading.text->isEmpty()) result.insert(key, *reading.text);
    return result;
}
QMap<QString, double> PresentationSnapshot::values() const {
    QMap<QString, double> result;
    for (const auto &entry : readings) {
        const auto &reading = entry.second;
        if (reading.fresh()) for (auto i = reading.values.cbegin(); i != reading.values.cend(); ++i) result.insert(i.key(), i.value());
    }
    return result;
}
PresentationSnapshot presentationSnapshot() {
    return {{
        {QStringLiteral("cpu"), cpuTempPresentation()}, {QStringLiteral("fans"), fansPresentation()},
        {QStringLiteral("storage"), storagePresentation()}, {QStringLiteral("power"), powerPresentation()},
        {QStringLiteral("igpu"), igpuPresentation()}, {QStringLiteral("battery"), batteryPresentation()}}};
}
QMap<QString, double> sensorValues() { return presentationSnapshot().values(); }

Opt cpuPackagePower() {
    // energy_uj is a free-running counter: power = Δenergy / Δt between two polls.
    struct Sample { long long uj = -1; qint64 ms = 0; };
    static QMutex mu;
    const QMutexLocker lock(&mu);
    static QHash<QString, Sample> last;
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    const QStringList zones = cachedDirs(QStringLiteral("/sys/class/powercap"), [](const QString &, const QString &e) {
        if (e.count(':') < 1 || e.count(':') > 2) return false;  // intel-rapl:0 (package), intel-rapl:0:0 (core)
        // intel-rapl-mmio mirrors the MSR package counter; showing both reads as two packages.
        return !(e.startsWith(QLatin1String("intel-rapl-mmio")) && QDir(QStringLiteral("/sys/class/powercap")).exists(QStringLiteral("intel-rapl:0")));
    });
    QStringList l;
    for (const QString &p : zones) {
        const auto uj = rdInt(p + "/energy_uj");
        const auto name = rd(p + "/name");  // zone names are short; kept per poll so a renamed zone shows up
        if (!uj || !name) continue;
        Sample &s = last[p];
        const qint64 now = clock.elapsed();
        if (s.uj >= 0 && now > s.ms && *uj >= s.uj) {
            const double w = double(*uj - s.uj) / double(now - s.ms) / 1000.0;
            l << QStringLiteral("%1 %2 W").arg(name->section('-', 0, 0), QString::number(w, 'f', 1));
        } else if (s.uj < 0) {
            l << QStringLiteral("%1 …").arg(name->section('-', 0, 0));
        }
        s = {*uj, now};
    }
    return joined(l);
}

Opt usbcInputs() {
    const QStringList ports = cachedDirs(QStringLiteral("/sys/class/power_supply"), [](const QString &, const QString &e) {
        return e.startsWith(QLatin1String("ucsi-source-psy"));
    });
    QStringList l;
    bool any = false;
    for (const QString &p : ports) {
        const QString e = p.section('/', -1);
        any = true;
        if (rdInt(p + "/online").value_or(0) != 1) continue;
        const auto uv = rdInt(p + "/voltage_now"), ua = rdInt(p + "/current_now"), max = rdInt(p + "/current_max");
        const QString port = QStringLiteral("Port %1").arg(e.section(':', -1).toInt());
        // current_max: what the charger agreed to deliver (USB PD contract), so "of N W" is the charger's limit at this voltage.
        const QString limit = uv && max && *uv > 0 && *max > 0 ? QStringLiteral(" of %1 W").arg(*uv / 1e6 * (*max / 1e6), 0, 'f', 0) : QString();
        if (uv && ua && *uv > 0 && *ua > 0)
            l << QStringLiteral("%1 %2 W%3 (%4 V)").arg(port).arg(*uv / 1e6 * (*ua / 1e6), 0, 'f', 1).arg(limit).arg(*uv / 1e6, 0, 'f', 0);
        else if (!limit.isEmpty())
            l << port + QStringLiteral(" connected, up to") + limit.mid(3);
        else
            l << port + QStringLiteral(" connected");
    }
    if (!any) return std::nullopt;  // no UCSI at all: row hidden
    return l.isEmpty() ? Opt(QStringLiteral("none")) : joined(l);
}

Opt gpuMode() {
    // Display-class PCI functions: an AMD one next to the NVIDIA one = hybrid.
    const QDir d(QStringLiteral("/sys/bus/pci/devices"));
    // iGPU = an AMD *or* Intel display function (Legion/LOQ ship both CPU vendors).
    bool igpu = false, nv = false;
    for (const QString &e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const QString p = d.filePath(e);
        if (!rd(p + "/class").value_or(QString()).startsWith(QStringLiteral("0x03"))) continue;
        const QString v = rd(p + "/vendor").value_or(QString());
        igpu |= v == QStringLiteral("0x1002") || v == QStringLiteral("0x8086");
        nv |= v == QStringLiteral("0x10de");
    }
    if (igpu && nv) return QStringLiteral("Hybrid (iGPU + dGPU)");
    if (nv) return QStringLiteral("dGPU only (MUX) — iGPU disabled in firmware");
    return std::nullopt;
}

Opt parseGpuName(const QByteArray &out) {
    const QString first = QString::fromUtf8(out).trimmed().section('\n', 0, 0).trimmed();
    return first.isEmpty() ? Opt() : Opt(first);
}

Opt parseGpuLive(const QByteArray &out) {
    const QStringList parts = QString::fromUtf8(out).trimmed().section('\n', 0, 0).split(',');
    if (parts.size() < 4) return std::nullopt;
    const char *suffix[] = {"°C", "W", "MHz", "% util"};
    QStringList b;
    for (int i = 0; i < 4; ++i)
        if (const QString v = parts[i].trimmed(); !v.isEmpty() && !v.contains("N/A")) b << v + QString::fromUtf8(suffix[i]);
    return joined(b);
}


static QString trimmed(const QString &p) { return rd(p).value_or(QString()).trimmed(); }

Opt schedulerSummary() {
    QStringList b;
    utsname u{};
    const QString ver = uname(&u) == 0 ? QString::fromUtf8(u.version) : QString();
    // 6.6 replaced CFS with EEVDF; BORE (CachyOS) is a layer on top of it, switched by this sysctl.
    b << QStringLiteral("EEVDF");
    if (QFile::exists(QStringLiteral("/proc/sys/kernel/sched_bore")))
        b.last() += trimmed(QStringLiteral("/proc/sys/kernel/sched_bore")) == QLatin1String("1") ? QStringLiteral(" + BORE")
                                                                                                    : QStringLiteral(" (BORE off)");
    for (const char *m : {"PREEMPT_RT", "PREEMPT_DYNAMIC", "PREEMPT_LAZY", "PREEMPT"})
        if (ver.contains(QLatin1String(m))) { b << QString::fromLatin1(m); break; }
    if (QDir(QStringLiteral("/sys/kernel/sched_ext")).exists()) b << QStringLiteral("sched_ext");
    return joined(b);
}

Opt cpuLayout() {
    const QString base = QStringLiteral("/sys/devices/system/cpu/");
    QSet<QString> cores;
    int threads = 0;
    QMap<QString, long long> l3;  // shared_cpu_list -> KiB, one entry per cache domain
    for (const QString &e : QDir(base).entryList({QStringLiteral("cpu[0-9]*")}, QDir::Dirs)) {
        const QString d = base + e;
        if (rd(d + QStringLiteral("/online")).value_or(QStringLiteral("1")).trimmed() == QLatin1String("0")) continue;
        ++threads;
        cores << trimmed(d + QStringLiteral("/topology/physical_package_id")) + ':' + trimmed(d + QStringLiteral("/topology/core_id"));
        for (const QString &c : QDir(d + QStringLiteral("/cache")).entryList({QStringLiteral("index[0-9]*")}, QDir::Dirs)) {
            const QString cd = d + QStringLiteral("/cache/") + c;
            if (trimmed(cd + QStringLiteral("/level")) != QLatin1String("3")) continue;
            QString sz = trimmed(cd + QStringLiteral("/size"));
            const bool m = sz.endsWith('M');
            sz.chop(1);
            l3.insert(trimmed(cd + QStringLiteral("/shared_cpu_list")), sz.toLongLong() * (m ? 1024 : 1));
        }
    }
    if (threads == 0) return std::nullopt;
    QStringList b{QStringLiteral("%1C/%2T").arg(cores.size()).arg(threads)};
    const QString smt = trimmed(base + QStringLiteral("smt/control"));
    if (!smt.isEmpty()) b << QStringLiteral("SMT ") + smt;
    if (!l3.isEmpty()) {
        QStringList sizes;
        for (long long kib : l3) sizes << QStringLiteral("%1 MiB").arg(kib / 1024);
        b << QStringLiteral("L3 ") + sizes.join(QStringLiteral(" + "));
    }
    return joined(b);
}

Opt cpuPowerSummary() {
    const QString pol = QStringLiteral("/sys/devices/system/cpu/cpufreq/policy0/");
    QStringList b;
    const QString drv = trimmed(pol + QStringLiteral("scaling_driver"));
    if (drv.isEmpty()) return std::nullopt;
    b << drv;
    if (const QString g = trimmed(pol + QStringLiteral("scaling_governor")); !g.isEmpty()) b << g;
    if (const QString e = trimmed(pol + QStringLiteral("energy_performance_preference")); !e.isEmpty()) b << QStringLiteral("EPP ") + e;
    QString boost = trimmed(QStringLiteral("/sys/devices/system/cpu/cpufreq/boost"));
    if (!boost.isEmpty()) b << QStringLiteral("boost ") + (boost == QLatin1String("1") ? QStringLiteral("on") : QStringLiteral("off"));
    return joined(b);
}

Opt memoryPolicy() {
    QStringList b;
    const QString thp = trimmed(QStringLiteral("/sys/kernel/mm/transparent_hugepage/enabled"));
    const int o = thp.indexOf('['), c = thp.indexOf(']');
    if (o >= 0 && c > o) b << QStringLiteral("THP ") + thp.mid(o + 1, c - o - 1);
    if (const QString sw = trimmed(QStringLiteral("/proc/sys/vm/swappiness")); !sw.isEmpty()) b << QStringLiteral("swappiness ") + sw;
    int zram = 0;
    for (const QString &l : rd(QStringLiteral("/proc/swaps")).value_or(QString()).split('\n'))
        if (l.startsWith(QLatin1String("/dev/zram"))) ++zram;
    if (zram) b << QStringLiteral("zram %1").arg(zram);
    return joined(b);
}

} // namespace sysinfo
